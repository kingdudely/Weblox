// VFS bootstrap for the clean C11 host: asset metadata, in-experience preload,
// appStorage files and the asset base URL.  Uses generated WC.h exports only.
#ifndef WEBLOX_VFS_H
#define WEBLOX_VFS_H

#include "host.h"

/* Env: WC_META (metadata JSON path, default data/meta.json), WC_CONTENT
 * (content root, default data/content), WC_ASSET_BASE (base URL), WC_NOREG
 * (skip metadata registration and use the embedded manifest).
 * Returns 0 on success, -1 on a fatal step; details are logged as [vfs]. */
int weblox_vfs_bootstrap(w2c_WebClient* inst);

/* Set just the asset base URL (also done by weblox_vfs_bootstrap). */
int weblox_vfs_set_base_url(w2c_WebClient* inst, const char* url);

#endif
