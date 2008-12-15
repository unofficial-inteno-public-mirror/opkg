/* opkg.c - the opkg  package management system

   Thomas Wood <thomas@openedhand.com>

   Copyright (C) 2008 OpenMoko Inc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
 */

#include <config.h>
#include <fnmatch.h>

#include "opkg.h"
#include "opkg_conf.h"
#include "args.h"

#include "opkg_install.h"
#include "opkg_configure.h"
#include "opkg_download.h"
#include "opkg_remove.h"
#include "opkg_upgrade.h"

#include "sprintf_alloc.h"
#include "file_util.h"

#include <libbb/libbb.h>

struct _opkg_t
{
  args_t *args;
  opkg_conf_t *conf;
  opkg_option_t *options;
};

#define opkg_assert(expr) if (!(expr)) { \
    printf ("opkg: file %s: line %d (%s): Assertation '%s' failed",\
            __FILE__, __LINE__, __PRETTY_FUNCTION__, # expr); abort (); }

#define progress(percent) if (progress_callback) progress_callback (opkg, percent, user_data);

/** Private Functions ***/


static int
opkg_configure_packages(opkg_conf_t *conf, char *pkg_name)
{
  pkg_vec_t *all;
  int i;
  pkg_t *pkg;
  int r, err = 0;

  all = pkg_vec_alloc ();
  pkg_hash_fetch_available (&conf->pkg_hash, all);

  for (i = 0; i < all->len; i++)
  {
    pkg = all->pkgs[i];

    if (pkg_name && fnmatch (pkg_name, pkg->name, 0))
      continue;

    if (pkg->state_status == SS_UNPACKED)
    {
      r = opkg_configure (conf, pkg);
      if (r == 0)
      {
        pkg->state_status = SS_INSTALLED;
        pkg->parent->state_status = SS_INSTALLED;
        pkg->state_flag &= ~SF_PREFER;
      }
      else
      {
        if (!err)
          err = r;
      }
    }
  }

  pkg_vec_free (all);
  return err;
}

struct _curl_cb_data
{
  opkg_progress_callback_t cb;
  opkg_t *opkg;
  void *user_data;
  int start_range;
  int finish_range;
};

int
curl_progress_cb (struct _curl_cb_data *cb_data,
                  double t,   /* dltotal */
                  double d,   /* dlnow */
                  double ultotal,
                  double ulnow)
{
  int p = (t) ? d * 100 / t : 0;
  static int prev = -1;

  /* prevent the same value being sent twice (can occur due to rounding) */
  if (p == prev)
    return 0;
  prev = p;

  if (t < 1)
    return 0;

  (cb_data->cb)(cb_data->opkg,
                cb_data->start_range + (d / t * ((cb_data->finish_range - cb_data->start_range))),
                cb_data->user_data);

  return 0;
}


/*** Public API ***/

opkg_package_t *
opkg_package_new ()
{

  opkg_package_t *p;

  p = malloc (sizeof (opkg_package_t));
  memset (p, 0, sizeof (opkg_package_t));

  return p;
}

opkg_package_t *
opkg_package_new_with_values (const char *name, const char *version,
    const char *arch, const char *desc, const char *tags, int installed)
{
  opkg_package_t *package;
  package = opkg_package_new ();

#define sstrdup(x) (x) ? strdup (x) : NULL;

  package->name = sstrdup (name);
  package->version = sstrdup (version);
  package->architecture = sstrdup (arch);
  package->description = sstrdup (desc);
  package->tags = sstrdup (tags);
  package->installed = (installed != 0);

  return package;
}

void
opkg_package_free (opkg_package_t *p)
{
  free (p->name);
  free (p->version);
  free (p->architecture);
  free (p->description);
  free (p->tags);

  free (p);
}

opkg_t *
opkg_new ()
{
  opkg_t *opkg;
  opkg = malloc (sizeof (opkg_t));

  opkg->args = malloc (sizeof (args_t));
  args_init (opkg->args);

  opkg->conf = malloc (sizeof (opkg_conf_t));
  opkg_conf_init (opkg->conf, opkg->args);

  opkg_init_options_array (opkg->conf, &opkg->options);
  return opkg;
}

void
opkg_free (opkg_t *opkg)
{
  opkg_assert (opkg != NULL);

  opkg_conf_deinit (opkg->conf);
  args_deinit (opkg->args);
}

int
opkg_read_config_files (opkg_t *opkg)
{
  args_t *a;
  opkg_conf_t *c;

  opkg_assert (opkg != NULL);

  a = opkg->args;
  c = opkg->conf;

  /* Unfortunatly, the easiest way to re-read the config files right now is to
   * throw away opkg->conf and start again */

  /* copy the settings we need to keep */
  a->autoremove = c->autoremove;
  a->force_depends = c->force_depends;
  a->force_defaults = c->force_defaults;
  a->force_overwrite = c->force_overwrite;
  a->force_downgrade = c->force_downgrade;
  a->force_reinstall = c->force_reinstall;
  a->force_removal_of_dependent_packages = c->force_removal_of_dependent_packages;
  a->force_removal_of_essential_packages = c->force_removal_of_essential_packages;
  a->nodeps = c->nodeps;
  a->noaction = c->noaction;
  a->query_all = c->query_all;
  a->multiple_providers = c->multiple_providers;
  a->verbosity = c->verbosity;

  if (c->offline_root)
  {
    if (a->offline_root) free (a->offline_root);
    a->offline_root = strdup (c->offline_root);
  }

  if (c->offline_root_pre_script_cmd)
  {
    if (a->offline_root_pre_script_cmd) free (a->offline_root_pre_script_cmd);
    a->offline_root_pre_script_cmd = strdup (c->offline_root_pre_script_cmd);
  }

  if (c->offline_root_post_script_cmd)
  {
    if (a->offline_root_post_script_cmd) free (a->offline_root_post_script_cmd);
    a->offline_root_post_script_cmd = strdup (c->offline_root_post_script_cmd);
  }

  /* throw away old opkg_conf and start again */
  opkg_conf_deinit (opkg->conf);
  opkg_conf_init (opkg->conf, opkg->args);

  free (opkg->options);
  opkg_init_options_array (opkg->conf, &opkg->options);

  return 0;
}

void
opkg_get_option (opkg_t *opkg, char *option, void **value)
{
  int i = 0;
  opkg_option_t *options;

  opkg_assert (opkg != NULL);
  opkg_assert (option != NULL);
  opkg_assert (value != NULL);

  options = opkg->options;

  /* look up the option
   * TODO: this would be much better as a hash table
   */
  while (options[i].name)
  {
    if (strcmp (options[i].name, option) != 0)
    {
      i++;
      continue;
    }
  }

  /* get the option */
  switch (options[i].type)
  {
  case OPKG_OPT_TYPE_BOOL:
    *((int *) value) = *((int *) options[i].value);
    return;

  case OPKG_OPT_TYPE_INT:
    *((int *) value) = *((int *) options[i].value);
    return;

  case OPKG_OPT_TYPE_STRING:
    *((char **)value) = strdup (options[i].value);
    return;
  }

}

void
opkg_set_option (opkg_t *opkg, char *option, void *value)
{
  int i = 0;
  opkg_option_t *options;

  opkg_assert (opkg != NULL);
  opkg_assert (option != NULL);
  opkg_assert (value != NULL);

  options = opkg->options;

  /* look up the option
   * TODO: this would be much better as a hash table
   */
  while (options[i].name)
  {
    if (strcmp (options[i].name, option) == 0)
    {
      break;
    }
    i++;
  }

  /* set the option */
  switch (options[i].type)
  {
  case OPKG_OPT_TYPE_BOOL:
    if (*((int *) value) == 0)
      *((int *)options[i].value) = 0;
    else
      *((int *)options[i].value) = 1;
    return;

  case OPKG_OPT_TYPE_INT:
    *((int *) options[i].value) = *((int *) value);
    return;

  case OPKG_OPT_TYPE_STRING:
    *((char **)options[i].value) = strdup (value);
    return;
  }

}

int
opkg_install_package (opkg_t *opkg, const char *package_name, opkg_progress_callback_t progress_callback, void *user_data)
{
  int err;
  char *package_id = NULL;

  opkg_assert (opkg != NULL);
  opkg_assert (package_name != NULL);

  progress (0);

  /* download the package */
  opkg_prepare_url_for_install (opkg->conf, package_name, &package_id);

  progress (50);

  /* ... */
  pkg_info_preinstall_check (opkg->conf);

  if (!package_id)
    package_id = strdup (package_name);

  /* unpack the package */
  if (opkg->conf->multiple_providers)
  {
    err = opkg_install_multi_by_name (opkg->conf, package_id);
  }
  else
  {
    err = opkg_install_by_name (opkg->conf, package_id);
  }

  if (err)
    return err;

  progress (75);

  /* run configure scripts, etc. */
  err = opkg_configure_packages (opkg->conf, NULL);
  if (err)
    return err;

  /* write out status files and file lists */
  opkg_conf_write_status_files (opkg->conf);
  pkg_write_changed_filelists (opkg->conf);

  progress (100);
  return 0;
}

int
opkg_remove_package (opkg_t *opkg, const char *package_name, opkg_progress_callback_t progress_callback, void *user_data)
{
  pkg_t *pkg = NULL;
  pkg_t *pkg_to_remove;

  opkg_assert (opkg != NULL);
  opkg_assert (package_name != NULL);

  progress (0);

  pkg_info_preinstall_check (opkg->conf);

  pkg_vec_t *installed_pkgs = pkg_vec_alloc ();

  pkg_hash_fetch_all_installed (&opkg->conf->pkg_hash, installed_pkgs);

  progress (25);

  pkg = pkg_hash_fetch_installed_by_name (&opkg->conf->pkg_hash, package_name);

  if (pkg == NULL)
  {
    /* XXX: Error: Package not installed. */
    return 1;
  }

  if (pkg->state_status == SS_NOT_INSTALLED)
  {
    /* XXX:  Error: Package seems to be not installed (STATUS = NOT_INSTALLED). */
    return 1;
  }

  progress (75);

  if (opkg->conf->restrict_to_default_dest)
  {
    pkg_to_remove = pkg_hash_fetch_installed_by_name_dest (&opkg->conf->pkg_hash,
                                                           pkg->name,
                                                           opkg->conf->default_dest);
  }
  else
  {
    pkg_to_remove = pkg_hash_fetch_installed_by_name (&opkg->conf->pkg_hash, pkg->name );
  }


  progress (75);

  opkg_remove_pkg (opkg->conf, pkg_to_remove, 0);

  /* write out status files and file lists */
  opkg_conf_write_status_files (opkg->conf);
  pkg_write_changed_filelists (opkg->conf);


  progress (100);
  return 0;
}

int
opkg_upgrade_package (opkg_t *opkg, const char *package_name, opkg_progress_callback_t progress_callback, void *user_data)
{
  pkg_t *pkg;

  opkg_assert (opkg != NULL);
  opkg_assert (package_name != NULL);

  progress (0);

  pkg_info_preinstall_check (opkg->conf);

  if (opkg->conf->restrict_to_default_dest)
  {
    pkg = pkg_hash_fetch_installed_by_name_dest (&opkg->conf->pkg_hash,
                                                 package_name,
                                                 opkg->conf->default_dest);
    if (pkg == NULL)
    {
      /* XXX: Error: Package not installed in default_dest */
      return 1;
    }
  }
  else
  {
    pkg = pkg_hash_fetch_installed_by_name (&opkg->conf->pkg_hash,
                                            package_name);
  }

  if (!pkg)
  {
    /* XXX: Error: Package not installed */
    return 1;
  }

  progress (25);

  opkg_upgrade_pkg (opkg->conf, pkg);
  progress (75);

  opkg_configure_packages (opkg->conf, NULL);
  progress (100);
  return 0;
}

int
opkg_upgrade_all (opkg_t *opkg, opkg_progress_callback_t progress_callback, void *user_data)
{
  pkg_vec_t *installed;
  int err = 0;
  int i;
  pkg_t *pkg;

  opkg_assert (opkg != NULL);
  progress (0);

  installed = pkg_vec_alloc ();
  pkg_info_preinstall_check (opkg->conf);

  pkg_hash_fetch_all_installed (&opkg->conf->pkg_hash, installed);
  for (i = 0; i < installed->len; i++)
  {
    pkg = installed->pkgs[i];
    err += opkg_upgrade_pkg (opkg->conf, pkg);
    progress (100 * i / installed->len);
  }
  pkg_vec_free (installed);

  if (err)
    return 1;

  err = opkg_configure_packages (opkg->conf, NULL);
  if (err)
    return 1;

  progress (100);
  return 0;
}

int
opkg_update_package_lists (opkg_t *opkg, opkg_progress_callback_t progress_callback, void *user_data)
{
  char *tmp;
  int err;
  char *lists_dir;
  pkg_src_list_elt_t *iter;
  pkg_src_t *src;
  int sources_list_count, sources_done;

  opkg_assert (opkg != NULL);

  progress (0);

  sprintf_alloc (&lists_dir, "%s",
                 (opkg->conf->restrict_to_default_dest)
                 ? opkg->conf->default_dest->lists_dir
                 : opkg->conf->lists_dir);

  if (!file_is_dir (lists_dir))
  {
    if (file_exists (lists_dir))
    {
      /* XXX: Error: file exists but is not a directory */
      free (lists_dir);
      return 1;
    }

    err = file_mkdir_hier (lists_dir, 0755);
    if (err)
    {
      /* XXX: Error: failed to create directory */
      free (lists_dir);
      return 1;
    }
  }

  tmp = strdup ("/tmp/opkg.XXXXXX");

  if (mkdtemp (tmp) == NULL)
  {
    /* XXX: Error: could not create temporary file name */
    free (lists_dir);
    free (tmp);
    return 1;
  }

  /* cout the number of sources so we can give some progress updates */
  sources_list_count = 0;
  sources_done = 0;
  iter = opkg->conf->pkg_src_list.head;
  while (iter)
  {
    sources_list_count++;
    iter = iter->next;
  }

  for (iter = opkg->conf->pkg_src_list.head; iter; iter = iter->next)
  {
    char *url, *list_file_name;

    src = iter->data;

    if (src->extra_data)  /* debian style? */
      sprintf_alloc (&url, "%s/%s/%s", src->value, src->extra_data,
                     src->gzip ? "Packages.gz" : "Packages");
    else
      sprintf_alloc (&url, "%s/%s", src->value, src->gzip ? "Packages.gz" : "Packages");

    sprintf_alloc (&list_file_name, "%s/%s", lists_dir, src->name);
    if (src->gzip)
    {
      char *tmp_file_name;
      FILE *in, *out;
      struct _curl_cb_data cb_data;

      sprintf_alloc (&tmp_file_name, "%s/%s.gz", tmp, src->name);

      /* XXX: Note: downloading url */

      cb_data.cb = progress_callback;
      cb_data.opkg = opkg;
      cb_data.user_data = user_data;
      cb_data.start_range = 100 * sources_done / sources_list_count;
      cb_data.finish_range = 100 * (sources_done + 1) / sources_list_count;

      err = opkg_download (opkg->conf, url, tmp_file_name, (curl_progress_func) curl_progress_cb, &cb_data);

      if (err == 0)
      {
        /* XXX: Note: Inflating downloaded file */
        in = fopen (tmp_file_name, "r");
        out = fopen (list_file_name, "w");
        if (in && out)
          unzip (in, out);
        else
          err = 1;
        if (in)
          fclose (in);
        if (out)
          fclose (out);
        unlink (tmp_file_name);
      }
    }
    else
      err = opkg_download (opkg->conf, url, list_file_name, NULL, NULL);

    if (err)
    {
      /* XXX: Error: download error */
    }
    free (url);

#ifdef HAVE_GPGME
    /* download detached signitures to verify the package lists */
    /* get the url for the sig file */
    if (src->extra_data)  /* debian style? */
      sprintf_alloc (&url, "%s/%s/%s", src->value, src->extra_data,
                     "Packages.sig");
    else
      sprintf_alloc (&url, "%s/%s", src->value, "Packages.sig");

    /* create temporary file for it */
    char *tmp_file_name;

    sprintf_alloc (&tmp_file_name, "%s/%s", tmp, "Packages.sig");

    err = opkg_download (opkg->conf, url, tmp_file_name, NULL, NULL);
    if (err)
    {
      /* XXX: Warning: Download failed */
    }
    else
    {
      int err;
      err = opkg_verify_file (opkg->conf, list_file_name, tmp_file_name);
      if (err == 0)
      {
        /* XXX: Notice: Signature check passed */
      }
      else
      {
        /* XXX: Warning: Signature check failed */
      }
    }
    unlink (tmp_file_name);
    free (tmp_file_name);
    free (url);
#else
    /* XXX: Note: Signiture check for %s skipped because GPG support was not
     * enabled in this build
     */
#endif
    free (list_file_name);

    sources_done++;
    progress (100 * sources_done / sources_list_count);
  }

  rmdir (tmp);
  free (tmp);
  free (lists_dir);

  return 0;
}


int
opkg_list_packages (opkg_t *opkg, opkg_package_callback_t callback, void *user_data)
{
  pkg_vec_t *all;
  int i;

  opkg_assert (opkg);
  opkg_assert (callback);

  all = pkg_vec_alloc ();
  pkg_hash_fetch_available (&opkg->conf->pkg_hash, all);
  for (i = 0; i < all->len; i++)
  {
    pkg_t *pkg;
    opkg_package_t *package;

    pkg = all->pkgs[i];

    package = opkg_package_new_with_values (
        pkg->name,
        pkg->version,
        pkg->architecture,
        pkg->description,
        pkg->tags,
        (pkg->state_status == SS_INSTALLED));

    callback (opkg, package, user_data);
  }

  pkg_vec_free (all);

  return 0;
}
