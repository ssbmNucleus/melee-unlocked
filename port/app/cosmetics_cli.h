// The offline cosmetic command line (cosmetics_cli.cpp), moved out of main.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include "host.h"
namespace cosmetics_cli {
int build_looks(const char* looks, const host::Options& o);   // MELEE_BUILD_LOOKS: the exit code
int run(const std::string& cosmetic_import, const std::string& cosmetic_scan_disc, const std::string& cosmetic_select,
        const std::string& cosmetic_deselect, bool cosmetic_deselect_requested, bool cosmetic_enable_effects,
        bool cosmetic_restore, bool cosmetic_status, const host::Options& o);   // the exit code
// --check-effect: one mod (a .dat, .usd, .zip or a folder of DATs) judged against --iso's disc and the
// catalog of --settings-path (host::cosmetics::check_effect), one JSON line on stdout. 0 it works, 1 it is
// refused (broken, not_an_effect), 2 not checked.
int check_effect(const std::string& path, const host::Options& o);
// --check-effects: every .dat, .usd and .zip under `folder` one mod each, a JSON line each, and with
// `csv` a file of rows file,verdict,reason,really,target,character,slots,sha256. 0 when every one was checked.
int check_effects(const std::string& folder, const std::string& csv, const host::Options& o);
}
