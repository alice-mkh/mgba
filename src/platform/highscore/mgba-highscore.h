#pragma once

#include <highscore/libhighscore.h>

G_BEGIN_DECLS

#define MGBA_TYPE_CORE (mgba_core_get_type())

G_DECLARE_FINAL_TYPE (mGBACore, mgba_core, MGBA, CORE, HsCore)

G_MODULE_EXPORT GType hs_get_core_type (void);

G_END_DECLS
