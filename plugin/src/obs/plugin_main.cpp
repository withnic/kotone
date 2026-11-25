#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("kotone", "en-US")

namespace kotone::obs_adapter {
void register_kotone_filter();
} // namespace kotone::obs_adapter

MODULE_EXPORT const char* obs_module_name(void) {
    return obs_module_text("Kotone.ModuleName");
}

MODULE_EXPORT const char* obs_module_description(void) {
    return obs_module_text("Kotone.ModuleDescription");
}

bool obs_module_load(void) {
    kotone::obs_adapter::register_kotone_filter();
    blog(LOG_INFO, "[kotone] Kotone voice filter module loaded (version 1.1.0)");
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[kotone] Kotone voice filter module unloaded");
}
