/*********************************************************
 *Copyright (C) 2004 Neil Horman
 *This program is free software; you can redistribute it and\or modify
 *it under the terms of the GNU General Public License as published 
 *by the Free Software Foundation; either version 2 of the License,
 *or  any later version.
 *
 *This program is distributed in the hope that it will be useful,
 *but WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *GNU General Public License for more details.
 *
 *File: yum.c
 *
 *Author:Neil Horman
 *
 *Date: 4/9/2015
 *
 *Description: yum package management implementation
 *********************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <manifest.h>
#include <package.h>
#include <freight-common.h>

static char worktemplate[256];
static char *workdir;
static char tmpdir[1024];

static void yum_cleanup()
{
	recursive_dir_cleanup(workdir);
	return;
}

static int build_path(const char *path, const char *prefix)
{

	printf("Path is %s, Prefix is %s\n", path, prefix);

	sprintf(tmpdir, "%s/%s%s%s",
		workdir,
		prefix ? prefix : "",
		prefix ? "/" : "",
		path);
	fprintf(stderr, "Building path %s\n", tmpdir);
	return mkdir(tmpdir, 0700);
}

static char *build_rpm_list(const struct manifest *manifest)
{
	size_t alloc_size = 0;
	int count;
	char *result;
	struct rpm *rpm = manifest->rpms;

	while (rpm) {
		/* Add an extra character for the space */
		alloc_size += strlen(rpm->name) + 2;
		rpm = rpm->next;
	}


	result =  calloc(1, alloc_size);
	if (!result)
		return NULL;

	rpm = manifest->rpms;
	count = 0;
	while (rpm) {
		/* Add 2 to include the trailing space */
		count += snprintf(&result[count], strlen(rpm->name)+2 , "%s ", rpm->name);
		rpm = rpm->next;
	}

	return result;
}

static int run_command(char *cmd, int print)
{
	int rc;
	FILE *yum_out;
	char buf[128];
	size_t count;
	
	yum_out = popen(cmd, "r");
	if (yum_out == NULL) {
		rc = errno;
		fprintf(stderr, "Unable to exec yum for install: %s\n", strerror(rc));
		return rc;
	}

	while(!feof(yum_out) && !ferror(yum_out)) {
		count = fread(buf, 1, 128, yum_out);
		if (print)
			fwrite(buf, count, 1, stderr);
	}

	rc = pclose(yum_out);

	if (rc == -1) {
		rc = errno;
		fprintf(stderr, "yum command failed: %s\n", strerror(rc));
	}

	return rc;
}

/*
 * Turn the srpm into an container rpm for freight
 */
static int yum_build_rpm(const struct manifest *manifest)
{
	char cmd[1024];
	char *output_path;
	char *quiet = manifest->opts.verbose ? "" : "--quiet";

	output_path = manifest->opts.output_path ? manifest->opts.output_path :
			workdir;

	/*
 	 * This will convert the previously built srpm into a binary rpm that
 	 * can serve as a containerized directory for systemd-nspawn
 	 */
	snprintf(cmd, 1024, "rpmbuild %s "
		 "-D\"_build_name_fmt "
		 "%s-freight-container-%s-%s.%%%%{ARCH}.rpm\" "
		 "-D\"__arch_install_post "
		 "/usr/lib/rpm/check-rpaths /usr/lib/rpm/check-buildroot\" "
		 "-D\"_rpmdir %s\" "
		 "--rebuild %s/%s-freight-container-%s-%s.src.rpm\n",
		 quiet, 
		 manifest->package.name, manifest->package.version,
		 manifest->package.release,
		 output_path, output_path,
		 manifest->package.name, manifest->package.version,
		 manifest->package.release);
	fprintf(stderr, "Building container binary rpm\n");
	return run_command(cmd, manifest->opts.verbose);
}

static int yum_init(const struct manifest *manifest)
{
	getcwd(worktemplate, 256);
	strcat(worktemplate, "/freight-builder.XXXXXX"); 

	workdir = mkdtemp(worktemplate);
	if (workdir == NULL) {
		fprintf(stderr, "Cannot create temporary work directory %s: %s\n",
			worktemplate, strerror(errno));
		return -EINVAL;
	}
	return 0;
}

static int stage_workdir(const struct manifest *manifest)
{
	struct repository *repo;
	FILE *repof;
	char *rpmlist;

	fprintf(stderr, "Initalizing work directory %s\n", workdir);

	if (build_path("", manifest->package.name)) {
		fprintf(stderr, "Cannot create container name directory: %s\n",
			strerror(errno));
		goto cleanup_tmpdir;
	}

	if (build_path("/containerfs", manifest->package.name)) {
		fprintf(stderr, "Cannot create containerfs directory: %s\n",
			strerror(errno));
		goto cleanup_tmpdir;
	}

	if (build_path("/containerfs/etc", manifest->package.name)) {
		fprintf(stderr, "Cannot create etc directory: %s\n",
			strerror(errno));
		goto cleanup_tmpdir;
	}

	if (build_path("/containerfs/etc/yum.repos.d", manifest->package.name)) {
		fprintf(stderr, "Cannot create repository directory: %s\n",
			strerror(errno)); 
		goto cleanup_tmpdir;
	}

	if (build_path("/containerfs/cache", manifest->package.name)) {
		fprintf(stderr, "Cannot create cache directory: %s\n",
			strerror(errno)); 
		goto cleanup_tmpdir;
	}

	if (build_path("/containerfs/logs", manifest->package.name)) {
		fprintf(stderr, "Cannot create log directory: %s\n",
			strerror(errno)); 
		goto cleanup_tmpdir;
	}

	/*
 	 * for each item in the repos list
 	 * lets create a file with that repository listed
 	 */
	repo = manifest->repos;
	while (repo) {
		sprintf(tmpdir, "%s/%s/containerfs/etc/yum.repos.d/%s-fb.repo",
			workdir, manifest->package.name, repo->name);
		repof = fopen(tmpdir, "w");
		if (!repof) {
			fprintf(stderr, "Error opening %s: %s\n",
				tmpdir, strerror(errno));
			goto cleanup_tmpdir;
		}

		fprintf(repof, "[%s-fb]\n", repo->name);
		fprintf(repof, "name=%s-fb\n", repo->name);
		fprintf(repof, "baseurl=%s\n", repo->url);
		fprintf(repof, "gpgcheck=0\n"); /* for now */
		fprintf(repof, "enabled=1\n");
		fclose(repof);
		repo = repo->next;
	}

	/*
 	 * create a base yum configuration
 	 */
	sprintf(tmpdir, "%s/%s/containerfs/etc/yum.conf",
		workdir,manifest->package.name);
	repof = fopen(tmpdir, "w");
	if (!repof) {
		fprintf(stderr, "Unable to create a repo configuration: %s\n",
			strerror(errno));
		goto cleanup_tmpdir;
	}
	fprintf(repof, "[main]\n");
	fprintf(repof, "cachedir=/cache\n");
	fprintf(repof, "keepcache=1\n");
	fprintf(repof, "logfile=/logs/yum.log\n");
	fprintf(repof, "reposdir=/etc/yum.repos.d/\n");
	fclose(repof);

	/*
 	 * Build the spec file
 	 */
	rpmlist = build_rpm_list(manifest);
	if (!rpmlist)
		goto cleanup_tmpdir;

	sprintf(tmpdir, "%s/%s-freight-container.spec", workdir,
		manifest->package.name);

	repof = fopen(tmpdir, "w");
	if (!repof) {
		fprintf(stderr, "Unable to create a spec file: %s\n",
			strerror(errno));
		goto cleanup_tmpdir;
	}

	/*
 	 * This builds out our spec file for the source RPM
 	 * We start with the usual tags
 	 */
	fprintf(repof, "Name: %s-freight-container\n",
		manifest->package.name);
	fprintf(repof, "Version: %s\n", manifest->package.version);
	fprintf(repof, "Release: %s\n", manifest->package.release);
	fprintf(repof, "License: %s\n", manifest->package.license);
	/*
 	 * We don't want these rpms to provide anything that the host system
 	 * might want
 	 */
	fprintf(repof, "AutoReqProv: no\n");
	fprintf(repof, "Summary: %s\n", manifest->package.summary);
	fprintf(repof, "Source0: %s-freight.tbz2\n", manifest->package.name);
	fprintf(repof, "\n\n");

	/*
 	 * buildrequires include yum as we're going to install a tree with it 
 	 */
	fprintf(repof, "BuildRequires: yum\n");

	fprintf(repof, "\n\n");
	fprintf(repof, "%%description\n");
	fprintf(repof, "A container rpm for freight\n");
	fprintf(repof, "\n\n");

	/*
 	 * The install section actually has yum do the install to
 	 * the srpms buildroot, that way we can package the containerized
 	 * fs into its own rpm
 	 */
	fprintf(repof, "%%install\n");
	fprintf(repof, "cd ${RPM_BUILD_ROOT}\n");
	fprintf(repof, "tar xvf %%{SOURCE0}\n");
	fprintf(repof, "yum -y --installroot=${RPM_BUILD_ROOT}/%s/containerfs/ "
		       " --releasever=%s install %s\n",
		manifest->package.name, manifest->yum.releasever, rpmlist); 
	free(rpmlist);
	fprintf(repof, "yum --installroot=${RPM_BUILD_ROOT}/%s/containerfs/ clean all\n",
		manifest->package.name);
	/*
 	 * After yum is done installing, we need to interrogate all the files
 	 * So that we can specify a file list in the %files section
 	 */
	fprintf(repof, "rm -f /tmp/%s.manifest\n", manifest->package.name);
	fprintf(repof, "for i in `find . -type d`\n");
	fprintf(repof, "do\n"); 
	fprintf(repof, "	echo \"%%dir /$i\" >> /tmp/%s.manifest\n",
		manifest->package.name);
	fprintf(repof, "done\n");

	fprintf(repof, "for i in `find . -type f`\n");
	fprintf(repof, "do\n"); 
	fprintf(repof, "	echo \"/$i\" >> /tmp/%s.manifest\n",
		manifest->package.name);
	fprintf(repof, "done\n");

	fprintf(repof, "for i in `find . -type l`\n");
	fprintf(repof, "do\n"); 
	fprintf(repof, "	echo \"/$i\" >> /tmp/%s.manifest\n",
		manifest->package.name);
	fprintf(repof, "done\n");

	fprintf(repof, "\n\n");
	fprintf(repof, "%%files -f /tmp/%s.manifest\n",
		manifest->package.name);

	/*
 	 * And an empty chagnelog
 	 */
	fprintf(repof, "\n\n");
	fprintf(repof, "%%changelog\n");
	fclose(repof);

	return 0;
cleanup_tmpdir:
	yum_cleanup();
	return -EINVAL;
}

/*
 * Build an srpm from our yum config setup we generated
 * in yum_init.
 */
static int yum_build_srpm(const struct manifest *manifest)
{
	int rc = -EINVAL;
	char cmd[1024];

	fprintf(stderr, "SRPM manifest name is %s\n", manifest->package.name);
	rc = stage_workdir(manifest);
	if (rc)
		goto out;

	/*
 	 * Tar up the etc directory we generated in our sandbox.  This gets 
 	 * Included in the srpm as Source0 of the spec file (written in
 	 * yum_init)
 	 */
	snprintf(cmd, 1024, "tar -C %s -jcf %s/%s-freight.tbz2 ./%s/\n",
		workdir, workdir, manifest->package.name, manifest->package.name);
	fprintf(stderr, "Creating yum configuration for container\n");
	rc = run_command(cmd, manifest->opts.verbose);
	if (rc)
		goto out;

	/*
 	 * Then build the srpms using the spec generated in yum_init.  Point our 
 	 * SOURCES dir at the sandbox to pick up the above tarball, and
 	 * optionally direct the srpm dir to the output directory if it was
 	 * specified
 	 */
	snprintf(cmd, 512, "rpmbuild -D \"_sourcedir %s\" -D \"_srcrpmdir %s\" "
		"-bs %s/%s-freight-container.spec\n",
		workdir,
		manifest->opts.output_path ? manifest->opts.output_path : workdir,
		workdir, manifest->package.name);
	fprintf(stderr, "Building container source rpm\n");
	rc = run_command(cmd, manifest->opts.verbose);
	if (rc)
		goto out;
out:
	return rc;
}

int yum_inspect(const struct manifest *mfst, const char *rpm)
{
	int rc = -EINVAL;
	char rpmcmd[1024];
	char *container_name = basename(rpm);
	char *tmp = strstr(container_name, "-freight-container");

	if (!container_name) {
		fprintf(stderr, "Unable to grab file name from path\n");
		goto out;
	}

	if (build_path("/introspect", NULL)) {
		fprintf(stderr, "unable to create introspect directory\n");
		goto out;
	}
	sprintf(rpmcmd, "yum --installroot=%s/introspect -y --nogpgcheck "
		"--releasever=%s install %s\n",
		workdir, mfst->yum.releasever, rpm);

	fprintf(stderr, "Unpacking container\n");
	rc = run_command(rpmcmd, mfst->opts.verbose);
	if (rc) {
		fprintf(stderr, "Unable to install container rpm\n");
		goto out;
	}
	*tmp = '\0'; /* Null Terminate the container name */
	fprintf(stderr, "Container name is %s\n", container_name);
	sprintf(rpmcmd, "yum --installroot %s/introspect/%s/containerfs/ --nogpgcheck check-update",
		 workdir, container_name);
	fprintf(stderr, "Looking for packages Requiring update:\n");
	rc = run_command(rpmcmd, 1);

	if (rc == 0)
		fprintf(stderr, "All packages up to date\n");
	/*
 	 * yum check-update exist with code 100 if there are updated packages
 	 * which is a success exit code to us
 	 */
	if (rc > 1)
		rc = 0;
out:
	return rc;
}

struct pkg_ops yum_ops = {
	.init = yum_init,
	.cleanup = yum_cleanup,
	.build_srpm = yum_build_srpm,
	.build_rpm = yum_build_rpm,
	.introspect_container = yum_inspect
};


