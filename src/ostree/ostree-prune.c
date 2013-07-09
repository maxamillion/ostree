/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

#include "ostree-prune.h"

typedef struct {
  OstreeRepo *repo;
  GHashTable *reachable;
  guint n_reachable_meta;
  guint n_reachable_content;
  guint n_unreachable_meta;
  guint n_unreachable_content;
  guint64 freed_bytes;
} OtPruneData;

static gboolean
maybe_prune_loose_object (OtPruneData        *data,
                          OstreePruneFlags    flags,
                          const char         *checksum,
                          OstreeObjectType    objtype,
                          GCancellable       *cancellable,
                          GError            **error)
{
  gboolean ret = FALSE;
  gs_unref_variant GVariant *key = NULL;
  gs_unref_object GFile *objf = NULL;

  key = ostree_object_name_serialize (checksum, objtype);

  objf = ostree_repo_get_object_path (data->repo, checksum, objtype);

  if (!g_hash_table_lookup_extended (data->reachable, key, NULL, NULL))
    {
      if (!(flags & OSTREE_PRUNE_FLAGS_NO_PRUNE))
        {
          gs_unref_object GFileInfo *info = NULL;

          if ((info = g_file_query_info (objf, OSTREE_GIO_FAST_QUERYINFO,
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                         cancellable, error)) == NULL)
            goto out;

          if (!gs_file_unlink (objf, cancellable, error))
            goto out;

          data->freed_bytes += g_file_info_get_size (info);
        }
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_unreachable_meta++;
      else
        data->n_unreachable_content++;
    }
  else
    {
      if (OSTREE_OBJECT_TYPE_IS_META (objtype))
        data->n_reachable_meta++;
      else
        data->n_reachable_content++;
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_prune (OstreeRepo        *repo,
              OstreePruneFlags   flags,
              gint               depth,
              gint              *out_objects_total,
              gint              *out_objects_pruned,
              guint64           *out_pruned_object_size_total,
              GCancellable      *cancellable,
              GError           **error)
{
  gboolean ret = FALSE;
  GHashTableIter hash_iter;
  gpointer key, value;
  gs_unref_hashtable GHashTable *objects = NULL;
  gs_unref_hashtable GHashTable *all_refs = NULL;
  gs_free char *formatted_freed_size = NULL;
  OtPruneData data;
  gboolean refs_only = flags & OSTREE_PRUNE_FLAGS_REFS_ONLY;

  memset (&data, 0, sizeof (data));

  data.repo = repo;
  data.reachable = ostree_repo_traverse_new_reachable ();

  if (refs_only)
    {
      if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                                  cancellable, error))
        goto out;
      
      g_hash_table_iter_init (&hash_iter, all_refs);
      
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          const char *checksum = value;
          
          if (!ostree_repo_traverse_commit (repo, checksum, depth, data.reachable,
                                            cancellable, error))
            goto out;
        }
    }

  if (!ostree_repo_list_objects (repo, OSTREE_REPO_LIST_OBJECTS_ALL, &objects,
                                 cancellable, error))
    goto out;

  if (!refs_only)
    {
      g_hash_table_iter_init (&hash_iter, objects);
      while (g_hash_table_iter_next (&hash_iter, &key, &value))
        {
          GVariant *serialized_key = key;
          const char *checksum;
          OstreeObjectType objtype;

          ostree_object_name_deserialize (serialized_key, &checksum, &objtype);

          if (objtype != OSTREE_OBJECT_TYPE_COMMIT)
            continue;
          
          if (!ostree_repo_traverse_commit (repo, checksum, depth, data.reachable,
                                            cancellable, error))
            goto out;
        }
    }

  g_hash_table_iter_init (&hash_iter, objects);
  while (g_hash_table_iter_next (&hash_iter, &key, &value))
    {
      GVariant *serialized_key = key;
      GVariant *objdata = value;
      const char *checksum;
      OstreeObjectType objtype;
      gboolean is_loose;
      
      ostree_object_name_deserialize (serialized_key, &checksum, &objtype);
      g_variant_get_child (objdata, 0, "b", &is_loose);

      if (!is_loose)
        continue;

      if (!maybe_prune_loose_object (&data, flags, checksum, objtype,
                                     cancellable, error))
        goto out;
    }
  ret = TRUE;
  *out_objects_total = (data.n_reachable_meta + data.n_unreachable_meta +
                        data.n_reachable_content + data.n_unreachable_content);
  *out_objects_pruned = (data.n_unreachable_meta + data.n_unreachable_content);
  *out_pruned_object_size_total = data.freed_bytes;
 out:
  if (data.reachable)
    g_hash_table_unref (data.reachable);
  return ret;
}
