// AppConfig.h - settings limits, validation and (de)serialisation. Pure; storage lives elsewhere.
#pragma once
#include "../core/AppTypes.h"
#include "../core/JsonWriter.h"
#include "../core/FlatJson.h"

struct Limits { int32_t lo, hi; };
extern const Limits LIM_PORTFOLIO, LIM_QUOTE, LIM_CHART, LIM_ROTATE, LIM_OFFSET, LIM_PAGES, LIM_CHARTS;

// Applies a settings patch (any subset of the API fields). All-or-nothing: on any error `s` is
// untouched and `err` holds a message. adminPass "" = unchanged; clearAdminPass:true removes it.
bool settingsApply(Settings &s, const FlatJson &patch, char *err, size_t errN, bool *hostnameChanged = nullptr);
bool settingsApplyJson(Settings &s, const char *json, size_t len, char *err, size_t errN, bool *hostnameChanged = nullptr);
bool validHostname(const char *h);

// `withSecrets` = persisted file (includes adminPass); the API passes false and gets adminPassSet.
void settingsWrite(JsonWriter &w, const Settings &s, bool withSecrets, bool withLimits);
// Loads the persisted file. Invalid fields keep their defaults (a corrupt value must never brick the device).
bool settingsLoadJson(Settings &s, const char *json, size_t len);
