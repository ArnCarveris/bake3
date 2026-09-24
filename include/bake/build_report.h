#ifndef BAKE3_BUILD_REPORT_H
#define BAKE3_BUILD_REPORT_H

#include "bake/context.h"

#define BAKE_REPORT_NO_STEP (-1)

#define BAKE_REPORT_KIND_COMPILE "compile"
#define BAKE_REPORT_KIND_LINK "link"
#define BAKE_REPORT_KIND_BUNDLE "bundle"
#define BAKE_REPORT_KIND_DISCOVERY "discovery"
#define BAKE_REPORT_KIND_GENERATE "generate"
#define BAKE_REPORT_KIND_ETC "etc"
#define BAKE_REPORT_KIND_PROJECT "project"
#define BAKE_REPORT_KIND_LOC "loc"
#define BAKE_REPORT_KIND_OTHER "other"

bake_build_report_t* bake_report_new(const bake_context_t *ctx, const char *path);

void bake_report_free(bake_build_report_t *report);

int32_t bake_report_current(const bake_build_report_t *report);

int32_t bake_report_open(
    bake_build_report_t *report,
    const char *kind,
    const char *name,
    const char *project);

int32_t bake_report_open_under(
    bake_build_report_t *report,
    int32_t parent,
    const char *kind,
    const char *name,
    const char *project);

void bake_report_set_object(
    bake_build_report_t *report,
    int32_t step,
    const char *object);

void bake_report_close(
    bake_build_report_t *report,
    int32_t step,
    bool ok,
    const char *error);

void bake_report_set_loc(
    bake_build_report_t *report,
    int32_t step,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank,
    const char *by_language_json,
    const char *by_file_json);

void bake_report_set_workspace_loc(
    bake_build_report_t *report,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank,
    const char *by_language_json);

void bake_report_set_loc_note(bake_build_report_t *report, const char *note);

int bake_report_finish(bake_build_report_t *report, bool ok);

#endif
