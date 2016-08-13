/*
 * Generated by gdbus-codegen 2.48.1. DO NOT EDIT.
 *
 * The license of this code is the same as for the source it was derived from.
 */

#ifndef __OSTREE_FETCH_SERVICE_H__
#define __OSTREE_FETCH_SERVICE_H__

#include <gio/gio.h>

G_BEGIN_DECLS


/* ------------------------------------------------------------------------ */
/* Declarations for ostree.fetch.service */

#define TYPE_OSTREE_FETCH_SERVICE (ostree_fetch_service_get_type ())
#define OSTREE_FETCH_SERVICE(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_OSTREE_FETCH_SERVICE, OstreeFetchService))
#define IS_OSTREE_FETCH_SERVICE(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_OSTREE_FETCH_SERVICE))
#define OSTREE_FETCH_SERVICE_GET_IFACE(o) (G_TYPE_INSTANCE_GET_INTERFACE ((o), TYPE_OSTREE_FETCH_SERVICE, OstreeFetchServiceIface))

struct _OstreeFetchService;
typedef struct _OstreeFetchService OstreeFetchService;
typedef struct _OstreeFetchServiceIface OstreeFetchServiceIface;

struct _OstreeFetchServiceIface
{
  GTypeInterface parent_iface;

  gboolean (*handle_fetch_config) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

  gboolean (*handle_fetch_delta_part) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *arg_path,
    guint64 arg_max_size);

  gboolean (*handle_fetch_delta_super) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *arg_name);

  gboolean (*handle_fetch_object) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    guint arg_objtype,
    const gchar *arg_checksum,
    guint64 arg_max_size);

  gboolean (*handle_fetch_ref) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *arg_name);

  gboolean (*handle_fetch_summary) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

  gboolean (*handle_fetch_summary_sig) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

  gboolean (*handle_new) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    gboolean arg_tls_permissive,
    const gchar *arg_tls_client_cert_path,
    const gchar *arg_tls_client_key_path,
    const gchar *arg_tls_ca_path,
    const gchar *arg_http_proxy);

  gboolean (*handle_open_metalink) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *arg_metalink_uri);

  gboolean (*handle_open_url) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *arg_base_uri);

  gboolean (*handle_progress) (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

};

GType ostree_fetch_service_get_type (void) G_GNUC_CONST;

GDBusInterfaceInfo *ostree_fetch_service_interface_info (void);
guint ostree_fetch_service_override_properties (GObjectClass *klass, guint property_id_begin);


/* D-Bus method call completion functions: */
void ostree_fetch_service_complete_fetch_object (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *temp_path);

void ostree_fetch_service_complete_fetch_delta_part (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *temp_path);

void ostree_fetch_service_complete_fetch_delta_super (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    GVariant *result);

void ostree_fetch_service_complete_fetch_ref (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    const gchar *result);

void ostree_fetch_service_complete_fetch_summary (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    GVariant *result);

void ostree_fetch_service_complete_fetch_summary_sig (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    GVariant *result);

void ostree_fetch_service_complete_fetch_config (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    guint remote_mode,
    gboolean has_tombstone_commits);

void ostree_fetch_service_complete_open_url (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

void ostree_fetch_service_complete_open_metalink (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

void ostree_fetch_service_complete_new (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation);

void ostree_fetch_service_complete_progress (
    OstreeFetchService *object,
    GDBusMethodInvocation *invocation,
    guint64 bytes_transferred);



/* D-Bus method calls: */
void ostree_fetch_service_call_fetch_object (
    OstreeFetchService *proxy,
    guint arg_objtype,
    const gchar *arg_checksum,
    guint64 arg_max_size,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_object_finish (
    OstreeFetchService *proxy,
    gchar **out_temp_path,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_object_sync (
    OstreeFetchService *proxy,
    guint arg_objtype,
    const gchar *arg_checksum,
    guint64 arg_max_size,
    gchar **out_temp_path,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_delta_part (
    OstreeFetchService *proxy,
    const gchar *arg_path,
    guint64 arg_max_size,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_delta_part_finish (
    OstreeFetchService *proxy,
    gchar **out_temp_path,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_delta_part_sync (
    OstreeFetchService *proxy,
    const gchar *arg_path,
    guint64 arg_max_size,
    gchar **out_temp_path,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_delta_super (
    OstreeFetchService *proxy,
    const gchar *arg_name,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_delta_super_finish (
    OstreeFetchService *proxy,
    GVariant **out_result,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_delta_super_sync (
    OstreeFetchService *proxy,
    const gchar *arg_name,
    GVariant **out_result,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_ref (
    OstreeFetchService *proxy,
    const gchar *arg_name,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_ref_finish (
    OstreeFetchService *proxy,
    gchar **out_result,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_ref_sync (
    OstreeFetchService *proxy,
    const gchar *arg_name,
    gchar **out_result,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_summary (
    OstreeFetchService *proxy,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_summary_finish (
    OstreeFetchService *proxy,
    GVariant **out_result,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_summary_sync (
    OstreeFetchService *proxy,
    GVariant **out_result,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_summary_sig (
    OstreeFetchService *proxy,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_summary_sig_finish (
    OstreeFetchService *proxy,
    GVariant **out_result,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_summary_sig_sync (
    OstreeFetchService *proxy,
    GVariant **out_result,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_fetch_config (
    OstreeFetchService *proxy,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_fetch_config_finish (
    OstreeFetchService *proxy,
    guint *out_remote_mode,
    gboolean *out_has_tombstone_commits,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_fetch_config_sync (
    OstreeFetchService *proxy,
    guint *out_remote_mode,
    gboolean *out_has_tombstone_commits,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_open_url (
    OstreeFetchService *proxy,
    const gchar *arg_base_uri,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_open_url_finish (
    OstreeFetchService *proxy,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_open_url_sync (
    OstreeFetchService *proxy,
    const gchar *arg_base_uri,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_open_metalink (
    OstreeFetchService *proxy,
    const gchar *arg_metalink_uri,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_open_metalink_finish (
    OstreeFetchService *proxy,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_open_metalink_sync (
    OstreeFetchService *proxy,
    const gchar *arg_metalink_uri,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_new (
    OstreeFetchService *proxy,
    gboolean arg_tls_permissive,
    const gchar *arg_tls_client_cert_path,
    const gchar *arg_tls_client_key_path,
    const gchar *arg_tls_ca_path,
    const gchar *arg_http_proxy,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_new_finish (
    OstreeFetchService *proxy,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_new_sync (
    OstreeFetchService *proxy,
    gboolean arg_tls_permissive,
    const gchar *arg_tls_client_cert_path,
    const gchar *arg_tls_client_key_path,
    const gchar *arg_tls_ca_path,
    const gchar *arg_http_proxy,
    GCancellable *cancellable,
    GError **error);

void ostree_fetch_service_call_progress (
    OstreeFetchService *proxy,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data);

gboolean ostree_fetch_service_call_progress_finish (
    OstreeFetchService *proxy,
    guint64 *out_bytes_transferred,
    GAsyncResult *res,
    GError **error);

gboolean ostree_fetch_service_call_progress_sync (
    OstreeFetchService *proxy,
    guint64 *out_bytes_transferred,
    GCancellable *cancellable,
    GError **error);



/* ---- */

#define TYPE_OSTREE_FETCH_SERVICE_PROXY (ostree_fetch_service_proxy_get_type ())
#define OSTREE_FETCH_SERVICE_PROXY(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_OSTREE_FETCH_SERVICE_PROXY, OstreeFetchServiceProxy))
#define OSTREE_FETCH_SERVICE_PROXY_CLASS(k) (G_TYPE_CHECK_CLASS_CAST ((k), TYPE_OSTREE_FETCH_SERVICE_PROXY, OstreeFetchServiceProxyClass))
#define OSTREE_FETCH_SERVICE_PROXY_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TYPE_OSTREE_FETCH_SERVICE_PROXY, OstreeFetchServiceProxyClass))
#define IS_OSTREE_FETCH_SERVICE_PROXY(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_OSTREE_FETCH_SERVICE_PROXY))
#define IS_OSTREE_FETCH_SERVICE_PROXY_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), TYPE_OSTREE_FETCH_SERVICE_PROXY))

typedef struct _OstreeFetchServiceProxy OstreeFetchServiceProxy;
typedef struct _OstreeFetchServiceProxyClass OstreeFetchServiceProxyClass;
typedef struct _OstreeFetchServiceProxyPrivate OstreeFetchServiceProxyPrivate;

struct _OstreeFetchServiceProxy
{
  /*< private >*/
  GDBusProxy parent_instance;
  OstreeFetchServiceProxyPrivate *priv;
};

struct _OstreeFetchServiceProxyClass
{
  GDBusProxyClass parent_class;
};

GType ostree_fetch_service_proxy_get_type (void) G_GNUC_CONST;

#if GLIB_CHECK_VERSION(2, 44, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeFetchServiceProxy, g_object_unref)
#endif

void ostree_fetch_service_proxy_new (
    GDBusConnection     *connection,
    GDBusProxyFlags      flags,
    const gchar         *name,
    const gchar         *object_path,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data);
OstreeFetchService *ostree_fetch_service_proxy_new_finish (
    GAsyncResult        *res,
    GError             **error);
OstreeFetchService *ostree_fetch_service_proxy_new_sync (
    GDBusConnection     *connection,
    GDBusProxyFlags      flags,
    const gchar         *name,
    const gchar         *object_path,
    GCancellable        *cancellable,
    GError             **error);

void ostree_fetch_service_proxy_new_for_bus (
    GBusType             bus_type,
    GDBusProxyFlags      flags,
    const gchar         *name,
    const gchar         *object_path,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data);
OstreeFetchService *ostree_fetch_service_proxy_new_for_bus_finish (
    GAsyncResult        *res,
    GError             **error);
OstreeFetchService *ostree_fetch_service_proxy_new_for_bus_sync (
    GBusType             bus_type,
    GDBusProxyFlags      flags,
    const gchar         *name,
    const gchar         *object_path,
    GCancellable        *cancellable,
    GError             **error);


/* ---- */

#define TYPE_OSTREE_FETCH_SERVICE_SKELETON (ostree_fetch_service_skeleton_get_type ())
#define OSTREE_FETCH_SERVICE_SKELETON(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), TYPE_OSTREE_FETCH_SERVICE_SKELETON, OstreeFetchServiceSkeleton))
#define OSTREE_FETCH_SERVICE_SKELETON_CLASS(k) (G_TYPE_CHECK_CLASS_CAST ((k), TYPE_OSTREE_FETCH_SERVICE_SKELETON, OstreeFetchServiceSkeletonClass))
#define OSTREE_FETCH_SERVICE_SKELETON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TYPE_OSTREE_FETCH_SERVICE_SKELETON, OstreeFetchServiceSkeletonClass))
#define IS_OSTREE_FETCH_SERVICE_SKELETON(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), TYPE_OSTREE_FETCH_SERVICE_SKELETON))
#define IS_OSTREE_FETCH_SERVICE_SKELETON_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), TYPE_OSTREE_FETCH_SERVICE_SKELETON))

typedef struct _OstreeFetchServiceSkeleton OstreeFetchServiceSkeleton;
typedef struct _OstreeFetchServiceSkeletonClass OstreeFetchServiceSkeletonClass;
typedef struct _OstreeFetchServiceSkeletonPrivate OstreeFetchServiceSkeletonPrivate;

struct _OstreeFetchServiceSkeleton
{
  /*< private >*/
  GDBusInterfaceSkeleton parent_instance;
  OstreeFetchServiceSkeletonPrivate *priv;
};

struct _OstreeFetchServiceSkeletonClass
{
  GDBusInterfaceSkeletonClass parent_class;
};

GType ostree_fetch_service_skeleton_get_type (void) G_GNUC_CONST;

#if GLIB_CHECK_VERSION(2, 44, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeFetchServiceSkeleton, g_object_unref)
#endif

OstreeFetchService *ostree_fetch_service_skeleton_new (void);


G_END_DECLS

#endif /* __OSTREE_FETCH_SERVICE_H__ */