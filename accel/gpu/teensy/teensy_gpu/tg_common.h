/*
 * tg_common.h -- include the shared headers from wherever this build keeps them.
 * Arduino (arduino-cli / IDE): build_teensy.cmd copies them into the sketch's src/ folder.
 * Simulator / host builds: -I../../common (see sim/Makefile).
 */
#ifndef TG_COMMON_H
#define TG_COMMON_H

#if defined(ARDUINO)
#include "src/gpu_proto.h"
#include "src/gpu_setup.h"
#include "src/geom.h"
#else
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "geom.h"
#endif
#include "tg_geom_ext.h"
#include "tg_plat.h"
#include "tg_core.h"

#endif
