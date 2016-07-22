/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011,2012,2013 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "libglnx.h"
#include "ostree.h"
#include "otutil.h"

#ifdef HAVE_LIBSOUP

#include "ostree-core-private.h"
#include "ostree-repo-private.h"
#include "ostree-repo-static-delta-private.h"
#include "ostree-metalink.h"
#include "ot-fs-utils.h"

#include <gio/gunixinputstream.h>

#define OSTREE_REPO_PULL_CONTENT_PRIORITY  (OSTREE_FETCHER_DEFAULT_PRIORITY)
#define OSTREE_REPO_PULL_METADATA_PRIORITY (OSTREE_REPO_PULL_CONTENT_PRIORITY - 100)

typedef enum {
  FETCH_METADATA,
  FETCH_CONTENT,
  FETCH_DELTAPART,
  FETCH_DELTASUPER,
  FETCH_REF,
  FETCH_SUMMARY,
  FETCH_SUMMARY_SIG,
  FETCH_CONFIG,
  FETCH_METALINK,
  MAX_FETCH_TYPES
} FetchType;

typedef struct {
  OstreeRepo   *repo;
  int           tmpdir_dfd;
  OstreeRepoPullFlags flags;
  char         *remote_name;
  OstreeFetcher *fetcher;
  GMainContext    *main_context;
  GCancellable *cancellable;

  OstreeAsyncProgress *progress;
  guint64           start_time;
  guint             n_outstanding[MAX_FETCH_TYPES];
  guint             n_outstanding_write_requests[MAX_FETCH_TYPES];
  guint             n_fetched[MAX_FETCH_TYPES];
  guint             n_requested[MAX_FETCH_TYPES];

  SoupURI          *base_uri;
  OstreeRepo       *remote_repo_local;
  gboolean          is_untrusted;
  OstreeRepoMode    remote_mode;
  gboolean          has_tombstone_commits;

  GBytes           *summary_data;
  GBytes           *summary_data_sig;
  GVariant         *summary;
  gboolean          gpg_verify_summary;
  gboolean          fetch_only_summary;

  gboolean          is_mirror;
  GHashTable       *requested_refs_to_fetch; /* Maps ref to commit */
  GHashTable       *expected_commit_sizes; /* Maps commit checksum to known size */
  GHashTable       *commits_to_fetch; /* Maps commit to itself */

  gboolean          dry_run;
  gboolean          dry_run_emitted_progress;
  gboolean          require_static_deltas;
  gboolean          disable_static_deltas;
  GHashTable       *summary_deltas_checksums;
  GPtrArray        *static_delta_superblocks;
  guint             n_total_deltaparts;
  guint64           total_deltapart_size;
  guint64           total_deltapart_usize;

  GQueue            scan_object_queue;
  GSource          *idle_src;
  GHashTable       *requested_metadata; /* Maps object name to itself */
  GHashTable       *scanned_metadata; /* Maps object name to itself */
  gint              n_scanned_metadata;

  char             *dir;
  GHashTable       *requested_content; /* Maps object name to itself */

  int               maxdepth;
  GHashTable       *commit_to_depth; /* Maps commit checksum maximum depth */
  gboolean          legacy_transaction_resuming;
  gboolean          commitpartial_exists;
  gboolean          gpg_verify;
  gboolean          is_commit_only;



  GError      **async_error;
  gboolean      caught_error;
  GError       *cached_async_error;
} OtPullData;

typedef struct {
  OtPullData       *pull_data;
  char             *checksum;
  OstreeObjectType  objtype;
} FetchObjectData;

typedef struct {
  OtPullData  *pull_data;
  GVariant *objects;
  char *expected_checksum;
} FetchStaticDeltaData;

typedef struct {
  OtPullData *pull_data;
  char *from_revision;
  char *to_revision;
  char *branch;
} FetchDeltaSuperBlockData;

typedef struct {
  guchar csum[OSTREE_SHA256_DIGEST_LEN];
  OstreeObjectType objtype;
  guint recursion_depth;
} ScanObjectQueueData;

static void
fetch_object_data_free (gpointer  data)
{
  FetchObjectData *fetch_data = data;
  g_free (fetch_data->checksum);
  g_free (fetch_data);
}

static void
fetch_static_delta_data_free (gpointer  data)
{
  FetchStaticDeltaData *fetch_data = data;
  g_free (fetch_data->expected_checksum);
  g_variant_unref (fetch_data->objects);
  g_free (fetch_data);
}

static void
fetch_delta_superblock_data_free (gpointer  data)
{
  FetchDeltaSuperBlockData *fetch_data = data;
  g_free (fetch_data->from_revision);
  g_free (fetch_data->to_revision);
  g_free (fetch_data->branch);
  g_free (fetch_data);
}

static void queue_scan_one_metadata_object_c (OtPullData         *pull_data,
                                              const guchar       *csum,
                                              OstreeObjectType    objtype,
                                              guint               recursion_depth);

static void
fetch_object (OtPullData      *pull_data,
              char            *checksum,
              OstreeObjectType objtype);

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...) G_GNUC_NULL_TERMINATED;

static SoupURI *
suburi_new (SoupURI   *base,
            const char *first,
            ...)
{
  va_list args;
  GPtrArray *arg_array;
  const char *arg;
  char *subpath;
  SoupURI *ret;

  arg_array = g_ptr_array_new ();
  g_ptr_array_add (arg_array, (char*)soup_uri_get_path (base));
  g_ptr_array_add (arg_array, (char*)first);

  va_start (args, first);
  
  while ((arg = va_arg (args, const char *)) != NULL)
    g_ptr_array_add (arg_array, (char*)arg);
  g_ptr_array_add (arg_array, NULL);

  subpath = g_build_filenamev ((char**)arg_array->pdata);
  g_ptr_array_unref (arg_array);
  
  ret = soup_uri_copy (base);
  soup_uri_set_path (ret, subpath);
  g_free (subpath);
  
  va_end (args);
  
  return ret;
}

static gboolean
update_progress (gpointer user_data)
{
  OtPullData *pull_data;
  guint64 bytes_transferred;
  guint n_scanned_metadata;
  guint64 start_time;

  guint outstanding_writes = 0;
  guint outstanding_fetches = 0;
  guint fetched = 0;
  guint requested = 0;
  int i;

  pull_data = user_data;

  if (! pull_data->progress)
    return G_SOURCE_REMOVE;

  for (i = 0; i < MAX_FETCH_TYPES; i++)
    {
      outstanding_writes += pull_data->n_outstanding_write_requests[i];
      outstanding_fetches += pull_data->n_outstanding[i];
      fetched += pull_data->n_fetched[i];
      requested += pull_data->n_requested[i];
    }

  if (pull_data->dry_run && outstanding_fetches != 0)
    return G_SOURCE_CONTINUE;

  bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
  n_scanned_metadata = pull_data->n_scanned_metadata;
  start_time = pull_data->start_time;

  ostree_async_progress_set_uint (pull_data->progress, "outstanding-fetches", outstanding_fetches);
  ostree_async_progress_set_uint (pull_data->progress, "outstanding-writes", outstanding_writes);
  ostree_async_progress_set_uint (pull_data->progress, "fetched", fetched);
  ostree_async_progress_set_uint (pull_data->progress, "requested", requested);
  ostree_async_progress_set_uint (pull_data->progress, "scanned-metadata", n_scanned_metadata);
  ostree_async_progress_set_uint64 (pull_data->progress, "bytes-transferred", bytes_transferred);
  ostree_async_progress_set_uint64 (pull_data->progress, "start-time", start_time);

  /* Deltas */
  ostree_async_progress_set_uint (pull_data->progress, "fetched-delta-parts",
                                  pull_data->n_fetched[FETCH_DELTAPART]);
  ostree_async_progress_set_uint (pull_data->progress, "total-delta-parts",
                                  pull_data->n_total_deltaparts);
  ostree_async_progress_set_uint64 (pull_data->progress, "total-delta-part-size",
                                    pull_data->total_deltapart_size);
  ostree_async_progress_set_uint64 (pull_data->progress, "total-delta-part-usize",
                                    pull_data->total_deltapart_usize);
  ostree_async_progress_set_uint (pull_data->progress, "total-delta-superblocks",
                                  pull_data->static_delta_superblocks->len);

  /* We fetch metadata before content.  These allow us to report metadata fetch progress specifically. */
  ostree_async_progress_set_uint (pull_data->progress, "outstanding-metadata-fetches", pull_data->n_outstanding[FETCH_METADATA]);
  ostree_async_progress_set_uint (pull_data->progress, "metadata-fetched", pull_data->n_fetched[FETCH_METADATA]);

  ostree_async_progress_set_status (pull_data->progress, NULL);

  if (pull_data->dry_run)
    {
      pull_data->dry_run_emitted_progress = TRUE;
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/* The core logic function for whether we should continue the main loop */
static gboolean
pull_termination_condition (OtPullData          *pull_data)
{
  gboolean current_idle = g_queue_is_empty (&pull_data->scan_object_queue);
  int i;

  for(i = 0; i < MAX_FETCH_TYPES && current_idle; i++) {
    current_idle = current_idle && pull_data->n_outstanding[i] == 0
                                && pull_data->n_outstanding_write_requests[i] == 0;
  }

  if (pull_data->dry_run)
    current_idle = current_idle && pull_data->dry_run_emitted_progress;

  if (pull_data->caught_error)
    return TRUE;

  if (current_idle)
    {
      g_debug ("pull: idle, exiting mainloop");
      return TRUE;
    }

  return FALSE;
}

static void
check_outstanding_requests_handle_error (OtPullData          *pull_data,
                                         GError              *error)
{
  if (error)
    {
      if (!pull_data->caught_error)
        {
          pull_data->caught_error = TRUE;
          g_propagate_error (pull_data->async_error, error);
        }
      else
        {
          g_error_free (error);
        }
      g_cancellable_cancel (pull_data->cancellable);
    }
}

static gboolean
write_commitpartial_for (OtPullData *pull_data,
                         const char *checksum,
                         GError **error)
{
  g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (checksum);
  glnx_fd_close int fd = -1;

  fd = openat (pull_data->repo->repo_dir_fd, commitpartial_path, O_EXCL | O_CREAT | O_WRONLY | O_CLOEXEC | O_NOCTTY, 0600);
  if (fd == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
scan_dirtree_object (OtPullData   *pull_data,
                     const char   *checksum,
                     int           recursion_depth,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  int i, n;
  g_autoptr(GVariant) tree = NULL;
  g_autoptr(GVariant) files_variant = NULL;
  g_autoptr(GVariant) dirs_variant = NULL;
  char *subdir_target = NULL;
  const char *dirname = NULL;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_DIR_TREE, checksum,
                                 &tree, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_TREE_VARIANT */
  files_variant = g_variant_get_child_value (tree, 0);
  dirs_variant = g_variant_get_child_value (tree, 1);

  n = g_variant_n_children (files_variant);
  for (i = 0; i < n; i++)
    {
      const char *filename;
      gboolean file_is_stored;
      g_autoptr(GVariant) csum = NULL;
      g_autofree char *file_checksum = NULL;

      g_variant_get_child (files_variant, i, "(&s@ay)", &filename, &csum);

      if (!ot_util_filename_validate (filename, error))
        goto out;

      /* Skip files if we're traversing a request only directory, unless it exactly
       * matches the path */
      if (pull_data->dir &&
          /* Should always an initial slash, we assert it in scan_dirtree_object */
          pull_data->dir[0] == '/' &&
          strcmp (pull_data->dir+1, filename) != 0)
        continue;

      file_checksum = ostree_checksum_from_bytes_v (csum);

      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, file_checksum,
                                   &file_is_stored, cancellable, error))
        goto out;

      if (!file_is_stored && pull_data->remote_repo_local)
        {
          if (!ostree_repo_import_object_from_with_trust (pull_data->repo, pull_data->remote_repo_local,
                                                          OSTREE_OBJECT_TYPE_FILE, file_checksum, !pull_data->is_untrusted,
                                                          cancellable, error))
            goto out;
        }
      else if (!file_is_stored && !g_hash_table_contains (pull_data->requested_content, file_checksum))
        {
          g_hash_table_add (pull_data->requested_content, g_strdup (file_checksum));
          fetch_object (pull_data, file_checksum, OSTREE_OBJECT_TYPE_FILE);
          file_checksum = NULL;  /* Transfer ownership */
        }
    }

    if (pull_data->dir)
      {
        const char *subpath = NULL;
        const char *nextslash = NULL;
        g_autofree char *dir_data = NULL;

        g_assert (pull_data->dir[0] == '/'); // assert it starts with / like "/usr/share/rpm"
        subpath = pull_data->dir + 1;  // refers to name minus / like "usr/share/rpm"
        nextslash = strchr (subpath, '/'); //refers to start of next slash like "/share/rpm"
        dir_data = pull_data->dir; // keep the original pointer around since strchr() points into it
        pull_data->dir = NULL;

        if (nextslash)
          {
            subdir_target = g_strndup (subpath, nextslash - subpath); // refers to first dir, like "usr"
            pull_data->dir = g_strdup (nextslash); // sets dir to new deeper level like "/share/rpm"
          }
        else // we're as deep as it goes, i.e. subpath = "rpm"
          subdir_target = g_strdup (subpath);
      }

  n = g_variant_n_children (dirs_variant);

  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) tree_csum = NULL;
      g_autoptr(GVariant) meta_csum = NULL;
      const guchar *tree_csum_bytes;
      const guchar *meta_csum_bytes;

      g_variant_get_child (dirs_variant, i, "(&s@ay@ay)",
                           &dirname, &tree_csum, &meta_csum);

      if (!ot_util_filename_validate (dirname, error))
        goto out;

      if (subdir_target && strcmp (subdir_target, dirname) != 0)
        continue;

      tree_csum_bytes = ostree_checksum_bytes_peek_validate (tree_csum, error);
      if (tree_csum_bytes == NULL)
        goto out;

      meta_csum_bytes = ostree_checksum_bytes_peek_validate (meta_csum, error);
      if (meta_csum_bytes == NULL)
        goto out;

      queue_scan_one_metadata_object_c (pull_data, tree_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1);
      queue_scan_one_metadata_object_c (pull_data, meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1);
    }

  ret = TRUE;
 out:
  return ret;
}


static gboolean
scan_commit_object (OtPullData         *pull_data,
                    const char         *checksum,
                    guint               recursion_depth,
                    GCancellable       *cancellable,
                    GError            **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) commit = NULL;
  g_autoptr(GVariant) parent_csum = NULL;
  const guchar *parent_csum_bytes = NULL;
  gpointer depthp;
  gint depth;

  if (recursion_depth > OSTREE_MAX_RECURSION)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exceeded maximum recursion");
      goto out;
    }

  if (g_hash_table_lookup_extended (pull_data->commit_to_depth, checksum,
                                    NULL, &depthp))
    {
      depth = GPOINTER_TO_INT (depthp);
    }
  else
    {
      depth = pull_data->maxdepth;
      g_hash_table_insert (pull_data->commit_to_depth, g_strdup (checksum),
                           GINT_TO_POINTER (depth));
    }

  if (pull_data->gpg_verify)
    {
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;

      result = _ostree_repo_verify_commit_internal (pull_data->repo,
                                                    checksum,
                                                    pull_data->remote_name,
                                                    NULL,
                                                    NULL,
                                                    cancellable,
                                                    error);

      if (result == NULL)
        goto out;

      /* Allow callers to output the results immediately. */
      g_signal_emit_by_name (pull_data->repo,
                             "gpg-verify-result",
                             checksum, result);

      if (ostree_gpg_verify_result_count_valid (result) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "GPG signatures found, but none are in trusted keyring");
          goto out;
        }
    }

  if (!ostree_repo_load_variant (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                 &commit, error))
    goto out;

  /* PARSE OSTREE_SERIALIZED_COMMIT_VARIANT */
  g_variant_get_child (commit, 1, "@ay", &parent_csum);
  if (g_variant_n_children (parent_csum) > 0)
    {
      parent_csum_bytes = ostree_checksum_bytes_peek_validate (parent_csum, error);
      if (parent_csum_bytes == NULL)
        goto out;
    }

  if (parent_csum_bytes != NULL && pull_data->maxdepth == -1)
    {
      queue_scan_one_metadata_object_c (pull_data, parent_csum_bytes,
                                        OSTREE_OBJECT_TYPE_COMMIT, recursion_depth + 1);
    }
  else if (parent_csum_bytes != NULL && depth > 0)
    {
      char parent_checksum[OSTREE_SHA256_STRING_LEN+1];
      gpointer parent_depthp;
      int parent_depth;

      ostree_checksum_inplace_from_bytes (parent_csum_bytes, parent_checksum);

      if (g_hash_table_lookup_extended (pull_data->commit_to_depth, parent_checksum,
                                        NULL, &parent_depthp))
        {
          parent_depth = GPOINTER_TO_INT (parent_depthp);
        }
      else
        {
          parent_depth = depth - 1;
        }

      if (parent_depth >= 0)
        {
          g_hash_table_insert (pull_data->commit_to_depth, g_strdup (parent_checksum),
                               GINT_TO_POINTER (parent_depth));
          queue_scan_one_metadata_object_c (pull_data, parent_csum_bytes,
                                            OSTREE_OBJECT_TYPE_COMMIT, recursion_depth + 1);
        }
    }

  if (!pull_data->is_commit_only)
    {
      g_autoptr(GVariant) tree_contents_csum = NULL;
      g_autoptr(GVariant) tree_meta_csum = NULL;
      const guchar *tree_contents_csum_bytes;
      const guchar *tree_meta_csum_bytes;

      g_variant_get_child (commit, 6, "@ay", &tree_contents_csum);
      g_variant_get_child (commit, 7, "@ay", &tree_meta_csum);

      tree_contents_csum_bytes = ostree_checksum_bytes_peek_validate (tree_contents_csum, error);
      if (tree_contents_csum_bytes == NULL)
        goto out;

      tree_meta_csum_bytes = ostree_checksum_bytes_peek_validate (tree_meta_csum, error);
      if (tree_meta_csum_bytes == NULL)
        goto out;

      queue_scan_one_metadata_object_c (pull_data, tree_contents_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_TREE, recursion_depth + 1);

      queue_scan_one_metadata_object_c (pull_data, tree_meta_csum_bytes,
                                        OSTREE_OBJECT_TYPE_DIR_META, recursion_depth + 1);
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
scan_one_metadata_object_c (OtPullData         *pull_data,
                            const guchar         *csum,
                            OstreeObjectType    objtype,
                            guint               recursion_depth,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) object = NULL;
  g_autofree char *tmp_checksum = NULL;
  gboolean is_requested;
  gboolean is_stored;

  tmp_checksum = ostree_checksum_from_bytes (csum);
  object = ostree_object_name_serialize (tmp_checksum, objtype);

  if (g_hash_table_contains (pull_data->scanned_metadata, object))
    return TRUE;

  is_requested = g_hash_table_contains (pull_data->requested_metadata, tmp_checksum);
  if (!ostree_repo_has_object (pull_data->repo, objtype, tmp_checksum, &is_stored,
                               cancellable, error))
    goto out;

  if (pull_data->remote_repo_local)
    {
      if (!is_stored)
        {
          if (!ostree_repo_import_object_from_with_trust (pull_data->repo, pull_data->remote_repo_local,
                                                          objtype, tmp_checksum, !pull_data->is_untrusted,
                                                          cancellable, error))
            goto out;
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
            {
              if (!write_commitpartial_for (pull_data, tmp_checksum, error))
                goto out;
            }
        }
      is_stored = TRUE;
      is_requested = TRUE;
    }

  if (!is_stored && !is_requested)
    {
      g_hash_table_add (pull_data->requested_metadata, g_strdup (tmp_checksum));

      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        fetch_object (pull_data, g_strdup (tmp_checksum), OSTREE_OBJECT_TYPE_COMMIT_META);
      fetch_object (pull_data, tmp_checksum, objtype);
      tmp_checksum = NULL;  /* Transfer ownership */
    }
  else if (objtype == OSTREE_OBJECT_TYPE_COMMIT && pull_data->is_commit_only)
    {
      if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                               pull_data->cancellable, error))
        goto out;

      g_hash_table_insert (pull_data->scanned_metadata, g_variant_ref (object), object);
      pull_data->n_scanned_metadata++;
    }
  else if (is_stored)
    {
      gboolean do_scan = pull_data->legacy_transaction_resuming || is_requested || pull_data->commitpartial_exists;

      /* For commits, always refetch detached metadata. */
      if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
        fetch_object (pull_data, g_strdup (tmp_checksum), OSTREE_OBJECT_TYPE_COMMIT_META);

      /* For commits, check whether we only had a partial fetch */
      if (!do_scan && objtype == OSTREE_OBJECT_TYPE_COMMIT)
        {
          OstreeRepoCommitState commitstate;

          if (!ostree_repo_load_commit (pull_data->repo, tmp_checksum, NULL, &commitstate, error))
            goto out;

          if (commitstate & OSTREE_REPO_COMMIT_STATE_PARTIAL)
            {
              do_scan = TRUE;
              pull_data->commitpartial_exists = TRUE;
            }
          else if (pull_data->maxdepth != 0)
            {
              /* Not fully accurate, but the cost here of scanning all
               * input commit objects if we're doing a depth fetch is
               * pretty low.  We'll do more accurate handling of depth
               * when parsing the actual commit.
               */
              do_scan = TRUE;
            }
        }

      if (do_scan)
        {
          switch (objtype)
            {
            case OSTREE_OBJECT_TYPE_COMMIT:
              if (!scan_commit_object (pull_data, tmp_checksum, recursion_depth,
                                       pull_data->cancellable, error))
                goto out;
              break;
            case OSTREE_OBJECT_TYPE_DIR_META:
              break;
            case OSTREE_OBJECT_TYPE_DIR_TREE:
              if (!scan_dirtree_object (pull_data, tmp_checksum, recursion_depth,
                                        pull_data->cancellable, error))
                goto out;
              break;
            default:
              g_assert_not_reached ();
              break;
            }
        }
      g_hash_table_add (pull_data->scanned_metadata, g_variant_ref (object));
      pull_data->n_scanned_metadata++;
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
idle_worker (gpointer user_data)
{
  OtPullData *pull_data = user_data;
  ScanObjectQueueData *scan_data;
  GError *error = NULL;

  scan_data = g_queue_pop_head (&pull_data->scan_object_queue);
  if (!scan_data)
    {
      g_clear_pointer (&pull_data->idle_src, (GDestroyNotify) g_source_destroy);
      return G_SOURCE_REMOVE;
    }

  scan_one_metadata_object_c (pull_data,
                              scan_data->csum,
                              scan_data->objtype,
                              scan_data->recursion_depth,
                              pull_data->cancellable,
                              &error);
  check_outstanding_requests_handle_error (pull_data, error);

  g_free (scan_data);
  return G_SOURCE_CONTINUE;
}

static void
ensure_idle_queued (OtPullData *pull_data)
{
  GSource *idle_src;

  if (pull_data->idle_src)
    return;

  idle_src = g_idle_source_new ();
  g_source_set_callback (idle_src, idle_worker, pull_data, NULL);
  g_source_attach (idle_src, pull_data->main_context);
  g_source_unref (idle_src);
  pull_data->idle_src = idle_src;
}

static void
queue_scan_one_metadata_object_c (OtPullData         *pull_data,
                                  const guchar         *csum,
                                  OstreeObjectType    objtype,
                                  guint               recursion_depth)
{
  ScanObjectQueueData *scan_data = g_new0 (ScanObjectQueueData, 1);

  memcpy (scan_data->csum, csum, sizeof (scan_data->csum));
  scan_data->objtype = objtype;
  scan_data->recursion_depth = recursion_depth;

  g_queue_push_tail (&pull_data->scan_object_queue, scan_data);
  ensure_idle_queued (pull_data);
}

static void
queue_scan_one_metadata_object (OtPullData         *pull_data,
                                const char         *csum,
                                OstreeObjectType    objtype,
                                guint               recursion_depth)
{
  guchar buf[OSTREE_SHA256_DIGEST_LEN];

  if (pull_data->dry_run)
    return;

  ostree_checksum_inplace_to_bytes (csum, buf);
  queue_scan_one_metadata_object_c (pull_data, buf, objtype, recursion_depth);
}

static void
on_metadata_written (GObject           *object,
                     GAsyncResult      *result,
                     gpointer           user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  const char *expected_checksum = fetch_data->checksum;
  OstreeObjectType objtype = fetch_data->objtype;
  g_autofree char *checksum = NULL;
  g_autofree guchar *csum = NULL;
  g_autofree char *stringified_object = NULL;

  if (!ostree_repo_write_metadata_finish ((OstreeRepo*)object, result,
                                          &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  g_assert (OSTREE_OBJECT_TYPE_IS_META (objtype));

  stringified_object = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", stringified_object);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted metadata object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  queue_scan_one_metadata_object_c (pull_data, csum, objtype, 0);

 out:
  pull_data->n_outstanding_write_requests[FETCH_METADATA]--;
  fetch_object_data_free (fetch_data);
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
content_fetch_on_write_complete (GObject        *object,
                                 GAsyncResult   *result,
                                 gpointer        user_data)
{
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  OstreeObjectType objtype = fetch_data->objtype;
  const char *expected_checksum = fetch_data->checksum;
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;
  g_autofree char *checksum_obj = NULL;

  if (!ostree_repo_write_content_finish ((OstreeRepo*)object, result,
                                         &csum, error))
    goto out;

  checksum = ostree_checksum_from_bytes (csum);

  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("write of %s complete", checksum_obj);

  if (strcmp (checksum, expected_checksum) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Corrupted content object; checksum expected='%s' actual='%s'",
                   expected_checksum, checksum);
      goto out;
    }

  pull_data->n_fetched[FETCH_CONTENT]++;
 out:
  pull_data->n_outstanding_write_requests[FETCH_CONTENT]--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  fetch_object_data_free (fetch_data);
}

static void
content_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GCancellable *cancellable = pull_data->cancellable;
  guint64 length;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autoptr(GVariant) xattrs = NULL;
  g_autoptr(GInputStream) file_in = NULL;
  g_autoptr(GInputStream) object_input = NULL;
  g_autofree char *temp_path = NULL;
  OstreeObjectType objtype = fetch_data->objtype;
  const char *checksum = fetch_data->checksum;
  g_autofree char *checksum_obj = NULL;
  gboolean free_fetch_data = TRUE;

  temp_path = _ostree_fetcher_request_uri_with_partial_finish (fetcher, result, error);
  if (!temp_path)
    goto out;

  g_assert (objtype == OSTREE_OBJECT_TYPE_FILE);

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s complete", checksum_obj);

  if (pull_data->is_mirror && pull_data->repo->mode == OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      gboolean have_object;
      if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_FILE, checksum,
                                   &have_object,
                                   cancellable, error))
        goto out;

      if (!have_object)
        {
          if (!_ostree_repo_commit_loose_final (pull_data->repo, checksum, OSTREE_OBJECT_TYPE_FILE,
                                                _ostree_fetcher_get_dfd (fetcher), temp_path,
                                                cancellable, error))
            goto out;
        }
      pull_data->n_fetched[FETCH_CONTENT]++;
    }
  else
    {
      /* Non-mirroring path */

      if (!ostree_content_file_parse_at (TRUE, _ostree_fetcher_get_dfd (fetcher),
                                         temp_path, FALSE,
                                         &file_in, &file_info, &xattrs,
                                         cancellable, error))
        {
          /* If it appears corrupted, delete it */
          (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);
          goto out;
        }

      /* Also, delete it now that we've opened it, we'll hold
       * a reference to the fd.  If we fail to write later, then
       * the temp space will be cleaned up.
       */
      (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);

      if (!ostree_raw_file_to_content_stream (file_in, file_info, xattrs,
                                              &object_input, &length,
                                              cancellable, error))
        goto out;

      pull_data->n_outstanding_write_requests[FETCH_CONTENT]++;
      ostree_repo_write_content_async (pull_data->repo, checksum,
                                       object_input, length,
                                       cancellable,
                                       content_fetch_on_write_complete, fetch_data);
      free_fetch_data = FALSE;
    }

 out:
  pull_data->n_outstanding[FETCH_CONTENT]--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (free_fetch_data)
    fetch_object_data_free (fetch_data);
}

static void
meta_fetch_on_complete (GObject           *object,
                        GAsyncResult      *result,
                        gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchObjectData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *temp_path = NULL;
  OstreeObjectType objtype = fetch_data->objtype;
  const char *checksum = fetch_data->checksum;
  g_autofree char *checksum_obj = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int fd = -1;
  gboolean free_fetch_data = TRUE;

  checksum_obj = ostree_object_to_string (checksum, objtype);
  g_debug ("fetch of %s complete", checksum_obj);

  temp_path = _ostree_fetcher_request_uri_with_partial_finish (fetcher, result, error);
  if (!temp_path)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT_META)
            {
              /* There isn't any detached metadata, not an error */
              g_clear_error (&local_error);
            }

          /* When traversing parents, do not fail on a missing commit.
           * We may be pulling from a partial repository that ends in
           * a dangling parent reference. */
          if (objtype == OSTREE_OBJECT_TYPE_COMMIT &&
                pull_data->maxdepth != 0)
            {
              g_clear_error (&local_error);
              /* If the remote repo supports tombstone commits, check if the commit was intentionally
                 deleted.  */
              if (pull_data->has_tombstone_commits)
                {
                  fetch_object (pull_data, g_strdup (checksum), OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT);
                }
            }
        }

      goto out;
    }

  /* Tombstone commits are always empty, so skip all processing here */
  if (objtype == OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT)
    goto out;

  fd = openat (_ostree_fetcher_get_dfd (fetcher), temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!ot_util_variant_map_fd (fd, 0, ostree_metadata_variant_type (objtype),
                                FALSE, &metadata, error))
    goto out;

  /* Now delete it, see comment in corresponding content fetch path */
  (void) unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0);

  /* Write the commitpartial file now while we're still fetching data */
  if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
    {
      if (!write_commitpartial_for (pull_data, checksum, error))
        goto out;
    }

  if (objtype == OSTREE_OBJECT_TYPE_COMMIT_META)
    {
      /* Special path for detached metadata because write_metadata_async doesn't support it */
      if (!ostree_repo_write_commit_detached_metadata (pull_data->repo, checksum, metadata,
                                                       pull_data->cancellable, error))
        goto out;
    }
  else
    {
      ostree_repo_write_metadata_async (pull_data->repo, objtype, checksum, metadata,
                                        pull_data->cancellable,
                                        on_metadata_written, fetch_data);
      pull_data->n_outstanding_write_requests[FETCH_METADATA]++;
    }
  free_fetch_data = FALSE;

 out:
  g_assert (pull_data->n_outstanding[FETCH_METADATA] > 0);
  pull_data->n_outstanding[FETCH_METADATA]--;
  pull_data->n_fetched[FETCH_METADATA]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (free_fetch_data)
    fetch_object_data_free (fetch_data);
}

/**
 * fetch_object:
 * @checksum: (transfer none): checksum of object
 * @objtype: object type
 *
 * Fetches the object of the given type and checksum
 */
static void
fetch_object (OtPullData        *pull_data,
              char              *checksum,
              OstreeObjectType   objtype)
{
  SoupURI *obj_uri = NULL;
  gboolean is_meta;
  FetchObjectData *fetch_data;
  g_autofree char *objpath = NULL;
  guint64 *expected_max_size_p;
  guint64 expected_max_size;

  g_debug ("queuing fetch of %s.%s", checksum,
           ostree_object_type_to_string (objtype));

  objpath = _ostree_get_relative_object_path (checksum, objtype, TRUE);
  obj_uri = suburi_new (pull_data->base_uri, objpath, NULL);

  is_meta = OSTREE_OBJECT_TYPE_IS_META (objtype);
  if (is_meta)
    {
      pull_data->n_outstanding[FETCH_METADATA]++;
      pull_data->n_requested[FETCH_METADATA]++;
    }
  else
    {
      pull_data->n_outstanding[FETCH_CONTENT]++;
      pull_data->n_requested[FETCH_CONTENT]++;
    }
  fetch_data = g_new0 (FetchObjectData, 1);
  fetch_data->pull_data = pull_data;
  fetch_data->checksum = checksum;
  fetch_data->objtype = objtype;

  expected_max_size_p = OSTREE_OBJECT_TYPE_IS_DETACHED(objtype) ? NULL
                          : g_hash_table_lookup (pull_data->expected_commit_sizes, checksum);
  if (expected_max_size_p)
    expected_max_size = *expected_max_size_p;
  else if (is_meta)
    expected_max_size = OSTREE_MAX_METADATA_SIZE;
  else
    expected_max_size = 0;

  _ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, obj_uri,
                                                  expected_max_size,
                                                  is_meta ? OSTREE_REPO_PULL_METADATA_PRIORITY
                                                          : OSTREE_REPO_PULL_CONTENT_PRIORITY,
                                                  pull_data->cancellable,
                                                  is_meta ? meta_fetch_on_complete : content_fetch_on_complete, fetch_data);
  soup_uri_free (obj_uri);
}

static void
on_static_delta_written (GObject           *object,
                         GAsyncResult      *result,
                         gpointer           user_data)
{
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;

  g_debug ("execute static delta part %s complete", fetch_data->expected_checksum);

  if (!_ostree_static_delta_part_execute_finish (pull_data->repo, result, error))
    goto out;

 out:
  g_assert (pull_data->n_outstanding_write_requests[FETCH_DELTAPART] > 0);
  pull_data->n_outstanding_write_requests[FETCH_DELTAPART]--;
  check_outstanding_requests_handle_error (pull_data, local_error);
  /* Always free state */
  fetch_static_delta_data_free (fetch_data);
}

static void
static_deltapart_fetch_on_complete (GObject           *object,
                                    GAsyncResult      *result,
                                    gpointer           user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchStaticDeltaData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *temp_path = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GVariant) part = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int fd = -1;
  gboolean free_fetch_data = TRUE;

  g_debug ("fetch static delta part %s complete", fetch_data->expected_checksum);

  temp_path = _ostree_fetcher_request_uri_with_partial_finish (fetcher, result, error);
  if (!temp_path)
    goto out;

  fd = openat (_ostree_fetcher_get_dfd (fetcher), temp_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* From here on, if we fail to apply the delta, we'll re-fetch it */
  if (unlinkat (_ostree_fetcher_get_dfd (fetcher), temp_path, 0) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  in = g_unix_input_stream_new (fd, FALSE);

  /* TODO - make async */
  if (!_ostree_static_delta_part_open (in, NULL, 0, fetch_data->expected_checksum,
                                       &part, pull_data->cancellable, error))
    goto out;

  _ostree_static_delta_part_execute_async (pull_data->repo,
                                           fetch_data->objects,
                                           part,
                                           /* Trust checksums if summary was gpg signed */
                                           pull_data->gpg_verify_summary && pull_data->summary_data_sig,
                                           pull_data->cancellable,
                                           on_static_delta_written,
                                           fetch_data);
  free_fetch_data = FALSE;
  pull_data->n_outstanding_write_requests[FETCH_DELTAPART]++;

 out:
  g_assert (pull_data->n_outstanding[FETCH_DELTAPART] > 0);
  pull_data->n_outstanding[FETCH_DELTAPART]--;
  pull_data->n_fetched[FETCH_DELTAPART]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (free_fetch_data)
    fetch_static_delta_data_free (fetch_data);
}

static gboolean
process_one_static_delta_fallback (OtPullData   *pull_data,
                                   gboolean      delta_byteswap,
                                   GVariant     *fallback_object,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) csum_v = NULL;
  g_autofree char *checksum = NULL;
  guint8 objtype_y;
  OstreeObjectType objtype;
  gboolean is_stored;
  guint64 compressed_size, uncompressed_size;

  g_variant_get (fallback_object, "(y@aytt)",
                 &objtype_y, &csum_v, &compressed_size, &uncompressed_size);
  if (!ostree_validate_structureof_objtype (objtype_y, error))
    goto out;
  if (!ostree_validate_structureof_csum_v (csum_v, error))
    goto out;

  compressed_size = maybe_swap_endian_u64 (delta_byteswap, compressed_size);
  uncompressed_size = maybe_swap_endian_u64 (delta_byteswap, uncompressed_size);

  pull_data->total_deltapart_size += compressed_size;
  pull_data->total_deltapart_usize += uncompressed_size;

  if (pull_data->dry_run)
    {
      ret = TRUE;
      goto out;
    }

  objtype = (OstreeObjectType)objtype_y;
  checksum = ostree_checksum_from_bytes_v (csum_v);

  if (!ostree_repo_has_object (pull_data->repo, objtype, checksum,
                               &is_stored,
                               cancellable, error))
    goto out;

  if (!is_stored)
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        {
          if (!g_hash_table_contains (pull_data->requested_metadata, checksum))
            {
              g_hash_table_add (pull_data->requested_metadata, g_strdup (checksum));

              /* Fetch metadata */
              if (objtype == OSTREE_OBJECT_TYPE_COMMIT)
                fetch_object (pull_data, g_strdup (checksum), OSTREE_OBJECT_TYPE_COMMIT_META);

              fetch_object (pull_data, checksum, objtype);
              checksum = NULL;  /* Transfer ownership */
            }
        }
      else
        {
          if (!g_hash_table_contains (pull_data->requested_content, checksum))
            {
              g_hash_table_add (pull_data->requested_content, g_strdup (checksum));
              fetch_object (pull_data, checksum, OSTREE_OBJECT_TYPE_FILE);
              checksum = NULL;  /* Transfer ownership */
            }
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static gboolean
process_one_static_delta (OtPullData   *pull_data,
                          const char   *from_revision,
                          const char   *to_revision,
                          GVariant     *delta_superblock,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  gboolean delta_byteswap;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) headers = NULL;
  g_autoptr(GVariant) fallback_objects = NULL;
  guint i, n;

  delta_byteswap = _ostree_delta_needs_byteswap (delta_superblock);

  /* Parsing OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT */
  metadata = g_variant_get_child_value (delta_superblock, 0);
  headers = g_variant_get_child_value (delta_superblock, 6);
  fallback_objects = g_variant_get_child_value (delta_superblock, 7);

  /* First process the fallbacks */
  n = g_variant_n_children (fallback_objects);
  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) fallback_object =
        g_variant_get_child_value (fallback_objects, i);

      if (!process_one_static_delta_fallback (pull_data, delta_byteswap,
                                              fallback_object,
                                              cancellable, error))
        goto out;
    }

  /* Write the to-commit object */
  if (!pull_data->dry_run)
  {
    g_autoptr(GVariant) to_csum_v = NULL;
    g_autofree char *to_checksum = NULL;
    g_autoptr(GVariant) to_commit = NULL;
    gboolean have_to_commit;

    to_csum_v = g_variant_get_child_value (delta_superblock, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      goto out;
    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    if (!ostree_repo_has_object (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                 &have_to_commit, cancellable, error))
      goto out;
    
    if (!have_to_commit)
      {
        FetchObjectData *fetch_data;
        g_autofree char *detached_path = _ostree_get_relative_static_delta_path (from_revision, to_revision, "commitmeta");
        g_autoptr(GVariant) detached_data = NULL;

        detached_data = g_variant_lookup_value (metadata, detached_path, G_VARIANT_TYPE("a{sv}"));
        if (detached_data && !ostree_repo_write_commit_detached_metadata (pull_data->repo,
                                                                          to_revision,
                                                                          detached_data,
                                                                          cancellable,
                                                                          error))
          goto out;

        fetch_data = g_new0 (FetchObjectData, 1);
        fetch_data->pull_data = pull_data;
        fetch_data->checksum = g_strdup (to_checksum);
        fetch_data->objtype = OSTREE_OBJECT_TYPE_COMMIT;

        to_commit = g_variant_get_child_value (delta_superblock, 4);

        ostree_repo_write_metadata_async (pull_data->repo, OSTREE_OBJECT_TYPE_COMMIT, to_checksum,
                                          to_commit,
                                          pull_data->cancellable,
                                          on_metadata_written, fetch_data);
        pull_data->n_outstanding_write_requests[FETCH_METADATA]++;
      }
  }

  n = g_variant_n_children (headers);
  pull_data->n_total_deltaparts += n;
  
  for (i = 0; i < n; i++)
    {
      const guchar *csum;
      g_autoptr(GVariant) header = NULL;
      gboolean have_all = FALSE;
      SoupURI *target_uri = NULL;
      g_autofree char *deltapart_path = NULL;
      FetchStaticDeltaData *fetch_data;
      g_autoptr(GVariant) csum_v = NULL;
      g_autoptr(GVariant) objects = NULL;
      g_autoptr(GBytes) inline_part_bytes = NULL;
      guint64 size, usize;
      guint32 version;
      const gboolean trusted = pull_data->gpg_verify_summary && pull_data->summary_data_sig;

      header = g_variant_get_child_value (headers, i);
      g_variant_get (header, "(u@aytt@ay)", &version, &csum_v, &size, &usize, &objects);

      version = maybe_swap_endian_u32 (delta_byteswap, version);
      size = maybe_swap_endian_u64 (delta_byteswap, size);
      usize = maybe_swap_endian_u64 (delta_byteswap, usize);

      if (version > OSTREE_DELTAPART_VERSION)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Delta part has too new version %u", version);
          goto out;
        }

      csum = ostree_checksum_bytes_peek_validate (csum_v, error);
      if (!csum)
        goto out;

      if (!_ostree_repo_static_delta_part_have_all_objects (pull_data->repo,
                                                            objects,
                                                            &have_all,
                                                            cancellable, error))
        goto out;

      if (have_all)
        {
          g_debug ("Have all objects from static delta %s-%s part %u",
                   from_revision ? from_revision : "empty", to_revision,
                   i);
          pull_data->n_fetched[FETCH_DELTAPART]++;
          continue;
        }

      deltapart_path = _ostree_get_relative_static_delta_part_path (from_revision, to_revision, i);

      { g_autoptr(GVariant) part_datav =
          g_variant_lookup_value (metadata, deltapart_path, G_VARIANT_TYPE ("(yay)"));

        if (part_datav)
          inline_part_bytes = g_variant_get_data_as_bytes (part_datav);
      }

      pull_data->total_deltapart_size += size;
      pull_data->total_deltapart_usize += usize;

      if (pull_data->dry_run)
        continue;
      
      fetch_data = g_new0 (FetchStaticDeltaData, 1);
      fetch_data->pull_data = pull_data;
      fetch_data->objects = g_variant_ref (objects);
      fetch_data->expected_checksum = ostree_checksum_from_bytes_v (csum_v);

      if (inline_part_bytes != NULL)
        {
          g_autoptr(GInputStream) memin = g_memory_input_stream_new_from_bytes (inline_part_bytes);
          g_autoptr(GVariant) inline_delta_part = NULL;

          /* For inline parts we are relying on per-commit GPG, so don't bother checksumming. */
          if (!_ostree_static_delta_part_open (memin, inline_part_bytes,
                                               OSTREE_STATIC_DELTA_OPEN_FLAGS_SKIP_CHECKSUM,
                                               NULL, &inline_delta_part,
                                               cancellable, error))
            goto out;
                                               
          _ostree_static_delta_part_execute_async (pull_data->repo,
                                                   fetch_data->objects,
                                                   inline_delta_part,
                                                   trusted,
                                                   pull_data->cancellable,
                                                   on_static_delta_written,
                                                   fetch_data);
          pull_data->n_outstanding_write_requests[FETCH_DELTAPART]++;
        }
      else
        {
          target_uri = suburi_new (pull_data->base_uri, deltapart_path, NULL);
          _ostree_fetcher_request_uri_with_partial_async (pull_data->fetcher, target_uri, size,
                                                          OSTREE_FETCHER_DEFAULT_PRIORITY,
                                                          pull_data->cancellable,
                                                          static_deltapart_fetch_on_complete,
                                                          fetch_data);
          pull_data->n_outstanding[FETCH_DELTAPART]++;
          soup_uri_free (target_uri);
        }
    }

  ret = TRUE;
 out:
  return ret;
}

static void
delta_superblock_process (FetchDeltaSuperBlockData *fetch_data,
                          GBytes *delta_superblock_data,
                          GCancellable  *cancellable,
                          GError       **error)
{
  OtPullData *pull_data = fetch_data->pull_data;
  char* from_revision = fetch_data->from_revision;
  char* to_revision = fetch_data->to_revision;
  if (delta_superblock_data)
    {
      g_autoptr(GVariant) delta_superblock = NULL;
      {
        g_autofree gchar *delta = NULL;
        g_autofree guchar *ret_csum = NULL;
        guchar *summary_csum;
        g_autoptr (GInputStream) summary_is = NULL;

        summary_is = g_memory_input_stream_new_from_data (g_bytes_get_data (delta_superblock_data, NULL),
                                                          g_bytes_get_size (delta_superblock_data),
                                                          NULL);

        if (!ot_gio_checksum_stream (summary_is, &ret_csum, cancellable, error))
          goto out;

        delta = g_strconcat (from_revision ? from_revision : "", from_revision ? "-" : "", to_revision, NULL);
        summary_csum = g_hash_table_lookup (pull_data->summary_deltas_checksums, delta);

        /* At this point we've GPG verified the data, so in theory
         * could trust that they provided the right data, but let's
         * make this a hard error.
         */
        if (pull_data->gpg_verify_summary && !summary_csum)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
            goto out;
          }

        if (summary_csum && memcmp (summary_csum, ret_csum, 32))
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid checksum for static delta %s", delta);
            goto out;
          }
      }

      delta_superblock = g_variant_new_from_bytes ((GVariantType*)OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT,
                                                      delta_superblock_data, FALSE);

      g_debug ("processing delta superblock for %s-%s", from_revision ? from_revision : "empty", to_revision);
      g_ptr_array_add (pull_data->static_delta_superblocks, g_variant_ref (delta_superblock));
      if (!process_one_static_delta (pull_data, from_revision, to_revision,
                                    delta_superblock,
                                    cancellable, error))
        goto out;
    }
  else
    {
      if (pull_data->require_static_deltas)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Static deltas required, but none found for %s to %s",
                        from_revision, to_revision);
          goto out;
        }
      g_debug ("no delta superblock for %s-%s", from_revision ? from_revision : "empty", to_revision);
      queue_scan_one_metadata_object (pull_data, to_revision, OSTREE_OBJECT_TYPE_COMMIT, 0);
    }
 out:
  return;
}

static void
delta_superblock_fetch_on_complete (GObject        *object,
                         GAsyncResult   *result,
                         gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchDeltaSuperBlockData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GInputStream* input = _ostree_fetcher_stream_uri_finish (fetcher, result, error);
  g_autoptr(GMemoryOutputStream) buf = NULL;
  GBytes* delta_superblock_data = NULL;

  if (!_ostree_fetcher_membuf_splice (input, FALSE, TRUE, &buf, pull_data->cancellable, error))
    goto out;

  if (buf)
    delta_superblock_data = g_memory_output_stream_steal_as_bytes ( buf );

  delta_superblock_process (fetch_data, delta_superblock_data, pull_data->cancellable, error);

 out:
  g_assert (pull_data->n_outstanding[FETCH_DELTASUPER] > 0);
  pull_data->n_outstanding[FETCH_DELTASUPER]--;
  pull_data->n_fetched[FETCH_DELTASUPER]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  fetch_delta_superblock_data_free (fetch_data);
}

/**
 * fetch_revision:
 * Fetch a static delta or commit file
 */
static void
fetch_revision (FetchDeltaSuperBlockData *fetch_data,
                GCancellable  *cancellable,
                GError       **error)
{
  OtPullData *pull_data = fetch_data->pull_data;
  gboolean free_fetch_data = TRUE;

  g_strchomp (fetch_data->to_revision);

  if (!ostree_validate_checksum_string (fetch_data->to_revision, error))
    goto out;

  /* Store actual resolved rev so we know which refs to update */
  g_hash_table_replace (pull_data->requested_refs_to_fetch, g_strdup (fetch_data->branch), g_strdup (fetch_data->to_revision));

  if (!pull_data->disable_static_deltas && (fetch_data->from_revision == NULL || g_strcmp0 (fetch_data->from_revision, fetch_data->to_revision) != 0))
    {
      g_autofree char *delta_name = _ostree_get_relative_static_delta_superblock_path (fetch_data->from_revision, fetch_data->to_revision);
      SoupURI *target_uri = suburi_new (pull_data->base_uri, delta_name, NULL);
      g_debug ("fetching delta %s", delta_name);
      _ostree_fetcher_stream_uri_async (pull_data->fetcher,
                                        target_uri,
                                        OSTREE_MAX_METADATA_SIZE,
                                        OSTREE_REPO_PULL_METADATA_PRIORITY,
                                        cancellable,
                                        delta_superblock_fetch_on_complete,
                                        fetch_data);
      free_fetch_data = FALSE;
      pull_data->n_outstanding[FETCH_DELTASUPER]++;
      g_clear_pointer (&target_uri, (GDestroyNotify) soup_uri_free);
    }
  else
    {
      delta_superblock_process (fetch_data, NULL, cancellable, error);
    }
 out:
  if (free_fetch_data)
    fetch_delta_superblock_data_free (fetch_data);
}

static void
revision_fetch_on_complete (GObject        *object,
                            GAsyncResult   *result,
                            gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  FetchDeltaSuperBlockData *fetch_data = user_data;
  OtPullData *pull_data = fetch_data->pull_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  GInputStream* input = _ostree_fetcher_stream_uri_finish (fetcher, result, error);
  gboolean free_fetch_data = TRUE;
  g_autoptr(GMemoryOutputStream) buf = NULL;

  if (!_ostree_fetcher_membuf_splice (input, TRUE, FALSE, &buf, pull_data->cancellable, error) || !buf)
    goto out;

  fetch_data->to_revision = g_memory_output_stream_steal_data ( buf );
  fetch_revision (fetch_data, pull_data->cancellable, error);
  free_fetch_data = FALSE;

 out:
  g_assert (pull_data->n_outstanding[FETCH_REF] > 0);
  pull_data->n_outstanding[FETCH_REF]--;
  pull_data->n_fetched[FETCH_REF]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
  if (free_fetch_data)
    fetch_delta_superblock_data_free (fetch_data);
}

static gboolean
lookup_commit_checksum_from_summary (GVariant      *summary,
                                     const char    *ref,
                                     char         **out_checksum,
                                     gsize         *out_size,
                                     GError       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GVariant) refs = g_variant_get_child_value (summary, 0);
  g_autoptr(GVariant) refdata = NULL;
  g_autoptr(GVariant) reftargetdata = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  guint64 commit_size;
  g_autoptr(GVariant) commit_csum_v = NULL;
  g_autoptr(GBytes) commit_bytes = NULL;
  int i;

  if (!ot_variant_bsearch_str (refs, ref, &i))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No such branch '%s' in repository summary",
                   ref);
      goto out;
    }

  refdata = g_variant_get_child_value (refs, i);
  reftargetdata = g_variant_get_child_value (refdata, 1);
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (!ostree_validate_structureof_csum_v (commit_csum_v, error))
    goto out;

  ret = TRUE;
  *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);
  *out_size = commit_size;
 out:
  return ret;
}

static gboolean
validate_variant_is_csum (GVariant       *csum,
                          GError        **error)
{
  gboolean ret = FALSE;

  if (!g_variant_is_of_type (csum, G_VARIANT_TYPE ("ay")))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Invalid checksum variant of type '%s', expected 'ay'",
                   g_variant_get_type_string (csum));
      goto out;
    }

  if (!ostree_validate_structureof_csum_v (csum, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

static gboolean
_ostree_repo_cache_summary (OstreeRepo        *self,
                            const char        *remote,
                            GBytes            *summary,
                            GBytes            *summary_sig,
                            GCancellable      *cancellable,
                            GError           **error)
{
  gboolean ret = FALSE;
  const char *summary_cache_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote);
  const char *summary_cache_sig_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote, ".sig");

  if (self->cache_dir_fd == -1)
    return TRUE;

  if (!glnx_shutil_mkdir_p_at (self->cache_dir_fd, _OSTREE_SUMMARY_CACHE_DIR, 0775, cancellable, error))
    goto out;

  if (!glnx_file_replace_contents_at (self->cache_dir_fd,
                                      summary_cache_file,
                                      g_bytes_get_data (summary, NULL),
                                      g_bytes_get_size (summary),
                                      self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    goto out;

  if (!glnx_file_replace_contents_at (self->cache_dir_fd,
                                      summary_cache_sig_file,
                                      g_bytes_get_data (summary_sig, NULL),
                                      g_bytes_get_size (summary_sig),
                                      self->disable_fsync ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;

}

/* Load the summary from the cache if the provided .sig file is the same as the
   cached version.  */
static gboolean
_ostree_repo_load_cache_summary_if_same_sig (OstreeRepo        *self,
                                             const char        *remote,
                                             GBytes            *summary_sig,
                                             GBytes            **summary,
                                             GCancellable      *cancellable,
                                             GError           **error)
{
  gboolean ret = FALSE;
  const char *summary_cache_sig_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote, ".sig");

  glnx_fd_close int prev_fd = -1;
  g_autoptr(GBytes) old_sig_contents = NULL;

  if (self->cache_dir_fd == -1)
    return TRUE;

  if (!ot_openat_ignore_enoent (self->cache_dir_fd, summary_cache_sig_file, &prev_fd, error))
    goto out;

  if (prev_fd < 0)
    {
      ret = TRUE;
      goto out;
    }

  old_sig_contents = glnx_fd_readall_bytes (prev_fd, cancellable, error);
  if (!old_sig_contents)
    goto out;

  if (g_bytes_compare (old_sig_contents, summary_sig) == 0)
    {
      const char *summary_cache_file = glnx_strjoina (_OSTREE_SUMMARY_CACHE_DIR, "/", remote);
      glnx_fd_close int summary_fd = -1;
      GBytes *summary_data;


      summary_fd = openat (self->cache_dir_fd, summary_cache_file, O_CLOEXEC | O_RDONLY);
      if (summary_fd < 0)
        {
          if (errno == ENOENT)
            {
              (void) unlinkat (self->cache_dir_fd, summary_cache_sig_file, 0);
              ret = TRUE;
              goto out;
            }

          glnx_set_error_from_errno (error);
          goto out;
        }

      summary_data = glnx_fd_readall_bytes (summary_fd, cancellable, error);
      if (!summary_data)
        goto out;
      *summary = summary_data;
    }
  ret = TRUE;

 out:
  return ret;
}

static OstreeFetcher *
_ostree_repo_remote_new_fetcher (OstreeRepo  *self,
                                 const char  *remote_name,
                                 GError     **error)
{
  OstreeFetcher *fetcher = NULL;
  OstreeFetcherConfigFlags fetcher_flags = 0;
  gboolean tls_permissive = FALSE;
  gboolean success = FALSE;

  g_return_val_if_fail (OSTREE_IS_REPO (self), NULL);
  g_return_val_if_fail (remote_name != NULL, NULL);

  if (!ostree_repo_get_remote_boolean_option (self, remote_name,
                                              "tls-permissive", FALSE,
                                              &tls_permissive, error))
    goto out;

  if (tls_permissive)
    fetcher_flags |= OSTREE_FETCHER_FLAGS_TLS_PERMISSIVE;

  fetcher = _ostree_fetcher_new (self->tmp_dir_fd, fetcher_flags);

  {
    g_autofree char *tls_client_cert_path = NULL;
    g_autofree char *tls_client_key_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-cert-path", NULL,
                                        &tls_client_cert_path, error))
      goto out;
    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-client-key-path", NULL,
                                        &tls_client_key_path, error))
      goto out;

    if ((tls_client_cert_path != NULL) != (tls_client_key_path != NULL))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Remote \"%s\" must specify both "
                     "\"tls-client-cert-path\" and \"tls-client-key-path\"",
                     remote_name);
        goto out;
      }
    else if (tls_client_cert_path != NULL)
      {
        g_autoptr(GTlsCertificate) client_cert = NULL;

        g_assert (tls_client_key_path != NULL);

        client_cert = g_tls_certificate_new_from_files (tls_client_cert_path,
                                                        tls_client_key_path,
                                                        error);
        if (client_cert == NULL)
          goto out;

        _ostree_fetcher_set_client_cert (fetcher, client_cert);
      }
  }

  {
    g_autofree char *tls_ca_path = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "tls-ca-path", NULL,
                                        &tls_ca_path, error))
      goto out;

    if (tls_ca_path != NULL)
      {
        g_autoptr(GTlsDatabase) db = NULL;

        db = g_tls_file_database_new (tls_ca_path, error);
        if (db == NULL)
          goto out;

        _ostree_fetcher_set_tls_database (fetcher, db);
      }
  }

  {
    g_autofree char *http_proxy = NULL;

    if (!ostree_repo_get_remote_option (self, remote_name,
                                        "proxy", NULL,
                                        &http_proxy, error))
      goto out;

    if (http_proxy != NULL)
      _ostree_fetcher_set_proxy (fetcher, http_proxy);
  }

  success = TRUE;

out:
  if (!success)
    g_clear_object (&fetcher);

  return fetcher;
}

static gboolean
process_summary (OtPullData    *pull_data,
                 GCancellable  *cancellable,
                 GError       **error)
{
  gboolean fetch_all_refs = pull_data->is_mirror && (g_hash_table_size (pull_data->requested_refs_to_fetch) == 0);
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gsize i, n;

  if (!pull_data->summary_data && pull_data->gpg_verify_summary)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "GPG verification enabled, but no summary found (use gpg-verify-summary=false in remote config to disable)");
      goto out;
    }

  if (!pull_data->summary_data && pull_data->require_static_deltas)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Fetch configured to require static deltas, but no summary found");
      goto out;
    }

  if (!pull_data->summary_data && fetch_all_refs)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Fetching all refs was requested in mirror mode, but remote repository does not have a summary");
      goto out;
    }

  if (pull_data->gpg_verify_summary && pull_data->summary_data && pull_data->summary_data_sig)
    {
      g_autoptr(GVariant) sig_variant = NULL;
      glnx_unref_object OstreeGpgVerifyResult *result = NULL;

      sig_variant = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT, pull_data->summary_data_sig, FALSE);
      result = _ostree_repo_gpg_verify_with_metadata (pull_data->repo,
                                                      pull_data->summary_data,
                                                      sig_variant,
                                                      pull_data->remote_name,
                                                      NULL,
                                                      NULL,
                                                      cancellable,
                                                      error);
      if (result == NULL)
        goto out;

      if (ostree_gpg_verify_result_count_valid (result) == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "GPG signatures found, but none are in trusted keyring");
          goto out;
        }
    }

  if (pull_data->summary_data)
    pull_data->summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, pull_data->summary_data, FALSE);

  if (pull_data->fetch_only_summary)
  {
    ret = TRUE;
    goto out;
  }

  if (pull_data->summary)
    {
      g_autoptr(GVariant) refs = NULL;
      g_autoptr(GVariant) deltas = NULL;
      g_autoptr(GVariant) additional_metadata = NULL;
      refs = g_variant_get_child_value (pull_data->summary, 0);
      n = g_variant_n_children (refs);
      for (i = 0; i < n; i++)
        {
          const char *refname;
          g_autoptr(GVariant) ref = g_variant_get_child_value (refs, i);

          g_variant_get_child (ref, 0, "&s", &refname);

          if (!ostree_validate_rev (refname, error))
            goto out;

          if (fetch_all_refs)
            g_hash_table_insert (pull_data->requested_refs_to_fetch, g_strdup (refname), NULL);
        }

      additional_metadata = g_variant_get_child_value (pull_data->summary, 1);
      deltas = g_variant_lookup_value (additional_metadata, OSTREE_SUMMARY_STATIC_DELTAS, G_VARIANT_TYPE ("a{sv}"));
      n = deltas ? g_variant_n_children (deltas) : 0;
      for (i = 0; i < n; i++)
        {
          const char *delta;
          GVariant *csum_v = NULL;
          guchar *csum_data = g_malloc (OSTREE_SHA256_DIGEST_LEN);
          g_autoptr(GVariant) ref = g_variant_get_child_value (deltas, i);

          g_variant_get_child (ref, 0, "&s", &delta);
          g_variant_get_child (ref, 1, "v", &csum_v);

          if (!validate_variant_is_csum (csum_v, error))
            goto out;

          memcpy (csum_data, ostree_checksum_bytes_peek (csum_v), 32);
          g_hash_table_insert (pull_data->summary_deltas_checksums,
                                g_strdup (delta),
                                csum_data);
        }
    }

  g_hash_table_iter_init (&hash_iter, pull_data->requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      char *ref = key;
      char *override_commitid = value;
      char *from_revision = NULL;
      FetchDeltaSuperBlockData* fetch_data = NULL;

      if (!ostree_repo_resolve_rev (pull_data->repo, ref, TRUE,
                                    &from_revision, error))
        goto out;

      fetch_data = g_new0 (FetchDeltaSuperBlockData, 1);
      fetch_data->pull_data = pull_data;
      fetch_data->from_revision = from_revision;
      fetch_data->branch = g_strdup(ref);

      /* Support specifying "" for an override commitid */
      if (override_commitid && *override_commitid)
        {
          fetch_data->to_revision = g_strdup (override_commitid);
          fetch_revision (fetch_data, cancellable, error);
        }
      else if (pull_data->summary)
        {
          gsize commit_size = 0;
          guint64 *malloced_size;

          if (!lookup_commit_checksum_from_summary (pull_data->summary, ref, &(fetch_data->to_revision), &commit_size, error))
            goto out;

          malloced_size = g_new0 (guint64, 1);
          *malloced_size = commit_size;
          g_hash_table_insert (pull_data->expected_commit_sizes, g_strdup (fetch_data->to_revision), malloced_size);

          fetch_revision (fetch_data, cancellable, error);
        }
      else
        {
          SoupURI *target_uri = suburi_new (pull_data->base_uri, "refs", "heads", ref, NULL);

          g_debug ("fetching ref %s", ref);

          pull_data->n_outstanding[FETCH_REF]++;
          _ostree_fetcher_stream_uri_async (pull_data->fetcher,
                                            target_uri,
                                            OSTREE_MAX_METADATA_SIZE,
                                            OSTREE_REPO_PULL_METADATA_PRIORITY,
                                            cancellable,
                                            revision_fetch_on_complete,
                                            fetch_data);

          g_clear_pointer (&target_uri, (GDestroyNotify) soup_uri_free);
        }

    }

  g_hash_table_iter_init (&hash_iter, pull_data->commits_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *commit = value;
      queue_scan_one_metadata_object (pull_data, commit, OSTREE_OBJECT_TYPE_COMMIT, 0);
    }

  ret = TRUE;
 out:
  return ret;
}

static void
summary_fetch_on_complete (GObject        *object,
                           GAsyncResult   *result,
                           gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  OtPullData *pull_data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;

  GInputStream* input = _ostree_fetcher_stream_uri_finish (fetcher, result, error);
  g_autoptr(GMemoryOutputStream) buf = NULL;

  if (!_ostree_fetcher_membuf_splice (input, FALSE, TRUE, &buf, pull_data->cancellable, error))
    goto out;

  if (buf)
    pull_data->summary_data = g_memory_output_stream_steal_as_bytes ( buf );

  if (pull_data->summary_data && pull_data->summary_data_sig)
    {
      if (!pull_data->remote_repo_local &&
          !_ostree_repo_cache_summary (pull_data->repo,
                                        pull_data->remote_name,
                                        pull_data->summary_data,
                                        pull_data->summary_data_sig,
                                        pull_data->cancellable,
                                        error))
        goto out;
    }

  process_summary (pull_data, pull_data->cancellable, error);

 out:
  g_assert (pull_data->n_outstanding[FETCH_SUMMARY] > 0);
  pull_data->n_outstanding[FETCH_SUMMARY]--;
  pull_data->n_fetched[FETCH_SUMMARY]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static gboolean
process_summary_sig (OtPullData    *pull_data,
                     GCancellable  *cancellable,
                     GError       **error)
{
  gboolean ret = FALSE;
  g_autoptr(GBytes) bytes_summary = NULL;

  if (!pull_data->summary_data_sig && pull_data->gpg_verify_summary)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "GPG verification enabled, but no summary.sig found (use gpg-verify-summary=false in remote config to disable)");
      goto out;
    }

  if (pull_data->summary_data_sig &&
      !pull_data->remote_repo_local &&
      !pull_data->summary_data &&
      !_ostree_repo_load_cache_summary_if_same_sig (pull_data->repo,
                                                    pull_data->remote_name,
                                                    pull_data->summary_data_sig,
                                                    &bytes_summary,
                                                    cancellable,
                                                    error))
    goto out;

  if (bytes_summary)
    pull_data->summary_data = g_bytes_ref (bytes_summary);

  if (pull_data->summary_data)
    {
      if (!process_summary (pull_data, cancellable, error))
        goto out;
    }
  else
    {
      SoupURI *uri = suburi_new (pull_data->base_uri, "summary", NULL);
      pull_data->n_outstanding[FETCH_SUMMARY]++;
      _ostree_fetcher_stream_uri_async (pull_data->fetcher,
                                        uri,
                                        OSTREE_MAX_METADATA_SIZE,
                                        OSTREE_REPO_PULL_METADATA_PRIORITY,
                                        cancellable,
                                        summary_fetch_on_complete,
                                        pull_data);
      soup_uri_free (uri);
    }
  ret = TRUE;
 out:
  return ret;
}

static void
summary_sig_fetch_on_complete (GObject        *object,
                               GAsyncResult   *result,
                               gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  OtPullData *pull_data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;

  GInputStream* input = _ostree_fetcher_stream_uri_finish (fetcher, result, error);
  g_autoptr(GMemoryOutputStream) buf = NULL;

  if (!_ostree_fetcher_membuf_splice (input, FALSE, TRUE, &buf, pull_data->cancellable, error))
    goto out;

  if (buf)
    pull_data->summary_data_sig = g_memory_output_stream_steal_as_bytes ( buf );

  process_summary_sig (pull_data, pull_data->cancellable, error);

 out:
  g_assert (pull_data->n_outstanding[FETCH_SUMMARY_SIG] > 0);
  pull_data->n_outstanding[FETCH_SUMMARY_SIG]--;
  pull_data->n_fetched[FETCH_SUMMARY_SIG]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static gboolean
fetch_summary_sig (OtPullData    *pull_data,
                   GCancellable  *cancellable,
                   GError       **error)
{
  SoupURI *uri = suburi_new (pull_data->base_uri, "summary.sig", NULL);
  pull_data->n_outstanding[FETCH_SUMMARY_SIG]++;
  _ostree_fetcher_stream_uri_async (pull_data->fetcher,
                                    uri,
                                    OSTREE_MAX_METADATA_SIZE,
                                    OSTREE_REPO_PULL_METADATA_PRIORITY,
                                    cancellable,
                                    summary_sig_fetch_on_complete,
                                    pull_data);
  soup_uri_free (uri);
  return TRUE;
}

static void
config_fetch_on_complete (GObject        *object,
                          GAsyncResult   *result,
                          gpointer        user_data)
{
  OstreeFetcher *fetcher = (OstreeFetcher *)object;
  OtPullData *pull_data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;
  g_autoptr(GBytes) bytes = NULL;
  g_autofree char *contents = NULL;
  gsize len;
  g_autoptr(GKeyFile) remote_config = g_key_file_new ();
  g_autofree char *remote_mode_str = NULL;

  GInputStream* input = _ostree_fetcher_stream_uri_finish (fetcher, result, error);
  g_autoptr(GMemoryOutputStream) buf = NULL;

  if (!_ostree_fetcher_membuf_splice (input, TRUE, FALSE, &buf, pull_data->cancellable, error))
    goto out;

  bytes = g_memory_output_stream_steal_as_bytes ( buf );
  contents = g_bytes_unref_to_data (bytes, &len);
  bytes = NULL;

  if (!g_key_file_load_from_data (remote_config, contents, len, 0, error))
    goto out;

  if (!ot_keyfile_get_value_with_default (remote_config, "core", "mode", "bare",
                                          &remote_mode_str, error))
    goto out;

  if (!ostree_repo_mode_from_string (remote_mode_str, &pull_data->remote_mode, error))
    goto out;

  if (!ot_keyfile_get_boolean_with_default (remote_config, "core", "tombstone-commits", FALSE,
                                            &pull_data->has_tombstone_commits, error))
    goto out;

  if (pull_data->remote_mode != OSTREE_REPO_MODE_ARCHIVE_Z2)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Can't pull from remote archives with mode \"%s\"",
                    remote_mode_str);
      goto out;
    }

  fetch_summary_sig (pull_data, pull_data->cancellable, error);

 out:
  g_assert (pull_data->n_outstanding[FETCH_CONFIG] > 0);
  pull_data->n_outstanding[FETCH_CONFIG]--;
  pull_data->n_fetched[FETCH_CONFIG]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

static void
fetch_config (OtPullData    *pull_data,
              GCancellable  *cancellable,
              GError       **error)
{
  if (strcmp (soup_uri_get_scheme (pull_data->base_uri), "file") == 0)
    {
      g_autoptr(GFile) remote_repo_path = g_file_new_for_path (soup_uri_get_path (pull_data->base_uri));
      pull_data->remote_repo_local = ostree_repo_new (remote_repo_path);
      if (!ostree_repo_open (pull_data->remote_repo_local, cancellable, error))
        return;
      pull_data->remote_mode = ostree_repo_get_mode (pull_data->remote_repo_local);
      if (!ot_keyfile_get_boolean_with_default (ostree_repo_get_config (pull_data->remote_repo_local), "core", "tombstone-commits", FALSE,
                                                &pull_data->has_tombstone_commits, error))
        return;
      fetch_summary_sig (pull_data, cancellable, error);
    }
  else
    {
      SoupURI *uri = suburi_new (pull_data->base_uri, "config", NULL);
      pull_data->n_outstanding[FETCH_CONFIG]++;
      _ostree_fetcher_stream_uri_async (pull_data->fetcher,
                                        uri,
                                        OSTREE_MAX_METADATA_SIZE,
                                        OSTREE_REPO_PULL_METADATA_PRIORITY,
                                        cancellable,
                                        config_fetch_on_complete,
                                        pull_data);
      soup_uri_free (uri);
    }
}

static void
metalink_fetch_on_complete (GObject        *object,
                            GAsyncResult   *result,
                            gpointer        user_data)
{
  OtPullData *pull_data = user_data;
  GError *local_error = NULL;
  GError **error = &local_error;

  FetchMetalinkResult* out = _ostree_metalink_request_finish (object, result, error);
  if (local_error != NULL)
      goto out;

  {
    g_autofree char *repo_base = g_path_get_dirname (soup_uri_get_path (out->target_uri));
    pull_data->base_uri = soup_uri_copy (out->target_uri);
    soup_uri_set_path (pull_data->base_uri, repo_base);
  }

  pull_data->summary_data = out->data;
  fetch_config (pull_data, pull_data->cancellable, error);

 out:
  g_assert (pull_data->n_outstanding[FETCH_METALINK] > 0);
  pull_data->n_outstanding[FETCH_METALINK]--;
  pull_data->n_fetched[FETCH_METALINK]++;
  check_outstanding_requests_handle_error (pull_data, local_error);
}

/* ------------------------------------------------------------------------------------------
 * Below is the libsoup-invariant API; these should match
 * the stub functions in the #else clause
 * ------------------------------------------------------------------------------------------
 */

/**
 * ostree_repo_pull_with_options:
 * @self: Repo
 * @remote_name: Name of remote
 * @options: A GVariant a{sv} with an extensible set of flags.
 * @progress: (allow-none): Progress
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like ostree_repo_pull(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 *   * refs (as): Array of string refs
 *   * flags (i): An instance of #OstreeRepoPullFlags
 *   * subdir (s): Pull just this subdirectory
 *   * override-remote-name (s): If local, add this remote to refspec
 *   * gpg-verify (b): GPG verify commits
 *   * gpg-verify-summary (b): GPG verify summary
 *   * depth (i): How far in the history to traverse; default is 0, -1 means infinite
 *   * disable-static-deltas (b): Do not use static deltas
 *   * require-static-deltas (b): Require static deltas
 *   * override-commit-ids (as): Array of specific commit IDs to fetch for refs
 *   * dry-run (b): Only print information on what will be downloaded (requires static deltas)
 *   * override-url (s): Fetch objects from this URL if remote specifies no metalink in options
 */
gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name_or_baseurl,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  g_autofree char *metalink_url_str = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;
  guint64 end_time;
  GSource *update_timeout = NULL;
  gsize i;

  g_autofree char **refs_to_fetch = NULL;
  OstreeRepoPullFlags flags = 0;
  const char *dir_to_pull = NULL;
  gboolean opt_gpg_verify = FALSE;
  gboolean opt_gpg_verify_summary = FALSE;
  char **override_commit_ids = NULL;
  const char *url_override = NULL;

  if (options)
    {
      int flags_i = OSTREE_REPO_PULL_FLAGS_NONE;
      (void) g_variant_lookup (options, "refs", "^a&s", &refs_to_fetch);
      (void) g_variant_lookup (options, "flags", "i", &flags_i);
      /* Reduce risk of issues if enum happens to be 64 bit for some reason */
      flags = flags_i;
      (void) g_variant_lookup (options, "subdir", "&s", &dir_to_pull);
      (void) g_variant_lookup (options, "override-remote-name", "s", &pull_data->remote_name);
      (void) g_variant_lookup (options, "gpg-verify", "b", &opt_gpg_verify);
      (void) g_variant_lookup (options, "gpg-verify-summary", "b", &opt_gpg_verify_summary);
      (void) g_variant_lookup (options, "depth", "i", &pull_data->maxdepth);
      (void) g_variant_lookup (options, "disable-static-deltas", "b", &pull_data->disable_static_deltas);
      (void) g_variant_lookup (options, "require-static-deltas", "b", &pull_data->require_static_deltas);
      (void) g_variant_lookup (options, "override-commit-ids", "^a&s", &override_commit_ids);
      (void) g_variant_lookup (options, "dry-run", "b", &pull_data->dry_run);
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
    }

  g_return_val_if_fail (pull_data->maxdepth >= -1, FALSE);
  if (refs_to_fetch && override_commit_ids)
    g_return_val_if_fail (g_strv_length (refs_to_fetch) == g_strv_length (override_commit_ids), FALSE);

  if (dir_to_pull)
    g_return_val_if_fail (dir_to_pull[0] == '/', FALSE);
  pull_data->dir = g_strdup (dir_to_pull);

  g_return_val_if_fail (!(pull_data->disable_static_deltas && pull_data->require_static_deltas), FALSE);
  /* We only do dry runs with static deltas, because we don't really have any
   * in-advance information for bare fetches.
   */
  g_return_val_if_fail (!pull_data->dry_run || pull_data->require_static_deltas, FALSE);

  pull_data->is_mirror = (flags & OSTREE_REPO_PULL_FLAGS_MIRROR) > 0;
  pull_data->is_commit_only = (flags & OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY) > 0;
  pull_data->is_untrusted = (flags & OSTREE_REPO_PULL_FLAGS_UNTRUSTED) > 0;

  if (error)
    pull_data->async_error = &pull_data->cached_async_error;
  else
    pull_data->async_error = NULL;
  pull_data->main_context = g_main_context_ref_thread_default ();
  pull_data->flags = flags;

  pull_data->repo = self;
  pull_data->progress = progress;
  pull_data->cancellable = cancellable;

  pull_data->expected_commit_sizes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                            (GDestroyNotify)g_free,
                                                            (GDestroyNotify)g_free);
  pull_data->commit_to_depth = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify)g_free,
                                                      NULL);
  pull_data->summary_deltas_checksums = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                               (GDestroyNotify)g_free,
                                                               (GDestroyNotify)g_free);
  pull_data->scanned_metadata = g_hash_table_new_full (ostree_hash_object_name, g_variant_equal,
                                                       (GDestroyNotify)g_variant_unref, NULL);
  pull_data->requested_content = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                        (GDestroyNotify)g_free, NULL);
  pull_data->requested_metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                         (GDestroyNotify)g_free, NULL);
  pull_data->requested_refs_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  pull_data->commits_to_fetch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init (&pull_data->scan_object_queue);

  pull_data->start_time = g_get_monotonic_time ();

  if (_ostree_repo_remote_name_is_file (remote_name_or_baseurl))
    {
      /* For compatibility with pull-local, don't gpg verify local
       * pulls by default.
       */
      pull_data->gpg_verify = opt_gpg_verify;
      pull_data->gpg_verify_summary = opt_gpg_verify_summary;

      if ((pull_data->gpg_verify || pull_data->gpg_verify_summary) &&
          pull_data->remote_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Must specify remote name to enable gpg verification");
          goto out;
        }
    }
  else
    {
      pull_data->remote_name = g_strdup (remote_name_or_baseurl);
      if (!ostree_repo_remote_get_gpg_verify (self, pull_data->remote_name,
                                              &pull_data->gpg_verify, error))
        goto out;
      if (!ostree_repo_remote_get_gpg_verify_summary (self, pull_data->remote_name,
                                                      &pull_data->gpg_verify_summary, error))
        goto out;
    }

  /* Create the state directory here - it's new with the commitpartial code,
   * and may not exist in older repositories.
   */
  if (mkdirat (pull_data->repo->repo_dir_fd, "state", 0777) != 0)
    {
      if (G_UNLIKELY (errno != EEXIST))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  pull_data->tmpdir_dfd = pull_data->repo->tmp_dir_fd;
  pull_data->static_delta_superblocks = g_ptr_array_new_with_free_func ((GDestroyNotify)g_variant_unref);

  {
    if (refs_to_fetch != NULL)
      {
        char **strviter = refs_to_fetch;
        char **commitid_strviter = override_commit_ids ? override_commit_ids : NULL;

        while (*strviter)
          {
            const char *branch = *strviter;

            if (ostree_validate_checksum_string (branch, NULL))
              {
                g_hash_table_add (pull_data->commits_to_fetch, g_strdup (branch));
              }
            else
              {
                char *commitid = commitid_strviter ? g_strdup (*commitid_strviter) : NULL;
                g_hash_table_insert (pull_data->requested_refs_to_fetch, g_strdup (branch), commitid);
              }

            strviter++;
            if (commitid_strviter)
              commitid_strviter++;
          }
      }
    else
      {
        g_autofree char **configured_branches = NULL;
        char **branches_iter;

        if (!ostree_repo_get_remote_list_option (self,
                                                remote_name_or_baseurl, "branches",
                                                &configured_branches, error))
          goto out;

        branches_iter = configured_branches;

        if (!(branches_iter && *branches_iter) && !pull_data->is_mirror)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "No configured branches for remote %s", remote_name_or_baseurl);
            goto out;
          }
        for (;branches_iter && *branches_iter; branches_iter++)
          {
            const char *branch = *branches_iter;

            g_hash_table_insert (pull_data->requested_refs_to_fetch, g_strdup (branch), NULL);
          }
      }
  }

  if (pull_data->progress)
    {
      update_timeout = g_timeout_source_new_seconds (pull_data->dry_run ? 0 : 1);
      g_source_set_priority (update_timeout, G_PRIORITY_HIGH);
      g_source_set_callback (update_timeout, update_progress, pull_data, NULL);
      g_source_attach (update_timeout, pull_data->main_context);
      g_source_unref (update_timeout);
    }

  if (!ostree_repo_prepare_transaction (pull_data->repo, &pull_data->legacy_transaction_resuming,
                                        cancellable, error))
    goto out;

  if (pull_data->legacy_transaction_resuming)
    g_debug ("resuming legacy transaction");

  pull_data->fetcher = _ostree_repo_remote_new_fetcher (self, remote_name_or_baseurl, error);
  if (pull_data->fetcher == NULL)
    goto out;

  /* At this point, all fields of pull_data are initialized besides:
   * - base_uri (filled in from metalink or ostree_repo_remote_get_url)
   * - summary* (filled in by fetching the summary or from cache)
   * - remote_repo_local, remote_mode, has_tombstone_commits (filled in from config)
   */

  if (!ostree_repo_get_remote_option (self,
                                      remote_name_or_baseurl, "metalink",
                                      NULL, &metalink_url_str, error))
    goto out;

  if (!metalink_url_str)
    {
      g_autofree char *baseurl = NULL;

      if (url_override != NULL)
        baseurl = g_strdup (url_override);
      else if (!ostree_repo_remote_get_url (self, remote_name_or_baseurl, &baseurl, error))
        goto out;

      pull_data->base_uri = soup_uri_new (baseurl);

      if (!pull_data->base_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to parse url '%s'", baseurl);
          goto out;
        }

      fetch_config (pull_data, cancellable, error);
    }
  else
    {
      SoupURI *metalink_uri = soup_uri_new (metalink_url_str);
      if (!metalink_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid metalink URL: %s", metalink_url_str);
          goto out;
        }
      pull_data->n_outstanding[FETCH_METALINK]++;
      _ostree_metalink_request_async (pull_data->fetcher,
                                      metalink_uri,
                                      "summary",
                                      OSTREE_MAX_METADATA_SIZE,
                                      OSTREE_REPO_PULL_METADATA_PRIORITY,
                                      metalink_fetch_on_complete,
                                      pull_data,
                                      cancellable);
      soup_uri_free (metalink_uri);
    }

  /* Now await work completion */
  while (!pull_termination_condition (pull_data))
    g_main_context_iteration (pull_data->main_context, TRUE);

  if (pull_data->caught_error)
    goto out;

  for(i = 0; i < MAX_FETCH_TYPES; i++) {
    g_assert_cmpint (pull_data->n_outstanding[i], ==, 0);
    g_assert_cmpint (pull_data->n_outstanding_write_requests[i], ==, 0);
  }

  if (pull_data->dry_run)
    {
      /* Skip updating refs */
      ret = TRUE;
      goto out;
    }

  g_hash_table_iter_init (&hash_iter, pull_data->requested_refs_to_fetch);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      g_autofree char *remote_ref = NULL;
      g_autofree char *original_rev = NULL;
          
      if (pull_data->remote_name)
        remote_ref = g_strdup_printf ("%s/%s", pull_data->remote_name, ref);
      else
        remote_ref = g_strdup (ref);

      if (!ostree_repo_resolve_rev (pull_data->repo, remote_ref, TRUE, &original_rev, error))
        goto out;
          
      if (original_rev && strcmp (checksum, original_rev) == 0)
        {
        }
      else
        {
          ostree_repo_transaction_set_ref (pull_data->repo, pull_data->is_mirror ? NULL : pull_data->remote_name,
                                          ref, checksum);
        }
    }

  if (pull_data->is_mirror && pull_data->summary_data)
    {
      if (!ot_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary",
                                        pull_data->summary_data, !pull_data->repo->disable_fsync,
                                        cancellable, error))
        goto out;

      if (pull_data->summary_data_sig &&
          !ot_file_replace_contents_at (pull_data->repo->repo_dir_fd, "summary.sig",
                                        pull_data->summary_data_sig, !pull_data->repo->disable_fsync,
                                        cancellable, error))
        goto out;
    }

  if (!ostree_repo_commit_transaction (pull_data->repo, NULL, cancellable, error))
    goto out;

  end_time = g_get_monotonic_time ();

  {
    guint64 bytes_transferred = _ostree_fetcher_bytes_transferred (pull_data->fetcher);
    if (bytes_transferred > 0 && pull_data->progress)
      {
        guint shift;
        GString *buf = g_string_new ("");

        if (bytes_transferred < 1024)
          shift = 1;
        else
          shift = 1024;

        if (pull_data->n_fetched[FETCH_DELTAPART] > 0)
          g_string_append_printf (buf, "%u delta parts, %u loose fetched",
                                  pull_data->n_fetched[FETCH_DELTAPART],
                                  pull_data->n_fetched[FETCH_METADATA] + pull_data->n_fetched[FETCH_CONTENT]);
        else
          g_string_append_printf (buf, "%u metadata, %u content objects fetched",
                                  pull_data->n_fetched[FETCH_METADATA], pull_data->n_fetched[FETCH_CONTENT]);

        g_string_append_printf (buf, "; %" G_GUINT64_FORMAT " %s transferred in %u seconds",
                                (guint64)(bytes_transferred / shift),
                                shift == 1 ? "B" : "KiB",
                                (guint) ((end_time - pull_data->start_time) / G_USEC_PER_SEC));

        ostree_async_progress_set_status (pull_data->progress, buf->str);
        g_string_free (buf, TRUE);
      }
  }

  /* iterate over commits fetched and delete any commitpartial files */
  if (!dir_to_pull && !pull_data->is_commit_only)
    {
      g_hash_table_iter_init (&hash_iter, pull_data->requested_refs_to_fetch);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (checksum);

          if (!ot_ensure_unlinked_at (pull_data->repo->repo_dir_fd, commitpartial_path, 0))
            goto out;
        }
      g_hash_table_iter_init (&hash_iter, pull_data->commits_to_fetch);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *commit = value;
          g_autofree char *commitpartial_path = _ostree_get_commitpartial_path (commit);

          if (!ot_ensure_unlinked_at (pull_data->repo->repo_dir_fd, commitpartial_path, 0))
            goto out;
        }
    }

  ret = TRUE;
 out:
  /* This is pretty ugly - we have two error locations, because we
   * have a mix of synchronous and async code.  Mixing them gets messy
   * as we need to avoid overwriting errors.
   */
  if (pull_data->cached_async_error && error && !*error)
    g_propagate_error (error, pull_data->cached_async_error);
  else
    g_clear_error (&pull_data->cached_async_error);
    
  ostree_repo_abort_transaction (pull_data->repo, cancellable, NULL);
  g_main_context_unref (pull_data->main_context);
  if (update_timeout)
    g_source_destroy (update_timeout);
  g_clear_object (&pull_data->fetcher);
  g_clear_object (&pull_data->remote_repo_local);
  g_free (pull_data->remote_name);
  g_free (pull_data->dir);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  g_clear_pointer (&pull_data->summary_data, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary_data_sig, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&pull_data->static_delta_superblocks, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->commit_to_depth, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->expected_commit_sizes, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->summary_deltas_checksums, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->idle_src, (GDestroyNotify) g_source_destroy);
  g_clear_pointer (&pull_data->requested_refs_to_fetch, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->commits_to_fetch, (GDestroyNotify) g_hash_table_unref);
  return ret;
}

/**
 * ostree_repo_remote_fetch_summary_with_options:
 * @self: Self
 * @name: name of a remote
 * @options: (nullable): A GVariant a{sv} with an extensible set of flags
 * @out_summary: (nullable): return location for raw summary data, or %NULL
 * @out_signatures: (nullable): return location for raw summary signature
 *                              data, or %NULL
 * @cancellable: a #GCancellable
 * @error: a #GError
 *
 * Like ostree_repo_remote_fetch_summary(), but supports an extensible set of flags.
 * The following are currently defined:
 *
 * - override-url (s): Fetch summary from this URL if remote specifies no metalink in options
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *remote_name_or_baseurl,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  gboolean ret = FALSE;
  g_autofree char *metalink_url_str = NULL;
  OtPullData pull_data_real = { 0, };
  OtPullData *pull_data = &pull_data_real;

  gboolean opt_gpg_verify_summary = FALSE;
  const char *url_override = NULL;
  int i;

  g_return_val_if_fail (OSTREE_REPO (self), FALSE);
  g_return_val_if_fail (remote_name_or_baseurl != NULL, FALSE);

  if (options)
    {
      (void) g_variant_lookup (options, "gpg-verify-summary", "b", &opt_gpg_verify_summary);
      (void) g_variant_lookup (options, "override-url", "&s", &url_override);
    }

  if (error)
    pull_data->async_error = &pull_data->cached_async_error;
  else
    pull_data->async_error = NULL;
  pull_data->main_context = g_main_context_ref_thread_default ();

  pull_data->repo = self;
  pull_data->cancellable = cancellable;

  if (_ostree_repo_remote_name_is_file (remote_name_or_baseurl))
    {
      /* For compatibility with pull-local, don't gpg verify local
       * pulls by default.
       */
      pull_data->gpg_verify_summary = opt_gpg_verify_summary;

      if (pull_data->gpg_verify_summary &&
          pull_data->remote_name == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Must specify remote name to enable gpg verification");
          goto out;
        }
    }
  else
    {
      pull_data->remote_name = g_strdup (remote_name_or_baseurl);
      if (!ostree_repo_remote_get_gpg_verify_summary (self, pull_data->remote_name,
                                                      &pull_data->gpg_verify_summary, error))
        goto out;
    }

  pull_data->tmpdir_dfd = pull_data->repo->tmp_dir_fd;
  pull_data->fetch_only_summary = TRUE;

  pull_data->fetcher = _ostree_repo_remote_new_fetcher (self, remote_name_or_baseurl, error);
  if (pull_data->fetcher == NULL)
    goto out;

  if (!ostree_repo_prepare_transaction (pull_data->repo, &pull_data->legacy_transaction_resuming,
                                        cancellable, error))
    goto out;

  if (!ostree_repo_get_remote_option (self,
                                      remote_name_or_baseurl, "metalink",
                                      NULL, &metalink_url_str, error))
    goto out;

  if (!metalink_url_str)
    {
      g_autofree char *baseurl = NULL;

      if (url_override != NULL)
        baseurl = g_strdup (url_override);
      else if (!ostree_repo_remote_get_url (self, remote_name_or_baseurl, &baseurl, error))
        goto out;

      pull_data->base_uri = soup_uri_new (baseurl);

      if (!pull_data->base_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Failed to parse url '%s'", baseurl);
          goto out;
        }

      fetch_config (pull_data, cancellable, error);
    }
  else
    {
      SoupURI *metalink_uri = soup_uri_new (metalink_url_str);
      if (!metalink_uri)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Invalid metalink URL: %s", metalink_url_str);
          goto out;
        }
      pull_data->n_outstanding[FETCH_METALINK]++;
      _ostree_metalink_request_async (pull_data->fetcher,
                                      metalink_uri,
                                      "summary",
                                      OSTREE_MAX_METADATA_SIZE,
                                      OSTREE_REPO_PULL_METADATA_PRIORITY,
                                      metalink_fetch_on_complete,
                                      pull_data,
                                      cancellable);
      soup_uri_free (metalink_uri);
    }

  /* Now await work completion */
  while (!pull_termination_condition (pull_data))
    g_main_context_iteration (pull_data->main_context, TRUE);

  if (pull_data->caught_error)
    goto out;

  for(i = 0; i < MAX_FETCH_TYPES; i++) {
    g_assert_cmpint (pull_data->n_outstanding[i], ==, 0);
    g_assert_cmpint (pull_data->n_outstanding_write_requests[i], ==, 0);
  }

  if (!ostree_repo_commit_transaction (pull_data->repo, NULL, cancellable, error))
    goto out;


  if (pull_data->summary_data != NULL)
    *out_summary = g_steal_pointer (&pull_data->summary_data);

  if (pull_data->summary_data_sig != NULL)
    *out_signatures = g_steal_pointer (&pull_data->summary_data_sig);

  ret = TRUE;
 out:
  /* This is pretty ugly - we have two error locations, because we
   * have a mix of synchronous and async code.  Mixing them gets messy
   * as we need to avoid overwriting errors.
   */
  if (pull_data->cached_async_error && error && !*error)
    g_propagate_error (error, pull_data->cached_async_error);
  else
    g_clear_error (&pull_data->cached_async_error);

  ostree_repo_abort_transaction (pull_data->repo, cancellable, NULL);
  g_main_context_unref (pull_data->main_context);
  g_clear_object (&pull_data->fetcher);
  g_clear_object (&pull_data->remote_repo_local);
  g_free (pull_data->remote_name);
  g_free (pull_data->dir);
  if (pull_data->base_uri)
    soup_uri_free (pull_data->base_uri);
  g_clear_pointer (&pull_data->summary_data, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary_data_sig, (GDestroyNotify) g_bytes_unref);
  g_clear_pointer (&pull_data->summary, (GDestroyNotify) g_variant_unref);
  g_clear_pointer (&pull_data->static_delta_superblocks, (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&pull_data->commit_to_depth, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->expected_commit_sizes, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->scanned_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->summary_deltas_checksums, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_content, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->requested_metadata, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->idle_src, (GDestroyNotify) g_source_destroy);
  g_clear_pointer (&pull_data->requested_refs_to_fetch, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&pull_data->commits_to_fetch, (GDestroyNotify) g_hash_table_unref);
  return ret;
}

#else /* HAVE_LIBSOUP */

gboolean
ostree_repo_pull_with_options (OstreeRepo             *self,
                               const char             *remote_name,
                               GVariant               *options,
                               OstreeAsyncProgress    *progress,
                               GCancellable           *cancellable,
                               GError                **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

gboolean
ostree_repo_remote_fetch_summary_with_options (OstreeRepo    *self,
                                               const char    *name,
                                               GVariant      *options,
                                               GBytes       **out_summary,
                                               GBytes       **out_signatures,
                                               GCancellable  *cancellable,
                                               GError       **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "This version of ostree was built without libsoup, and cannot fetch over HTTP");
  return FALSE;
}

#endif /* HAVE_LIBSOUP */
