// Exports clap_entry. clap-wrapper compiles this file once per format and links it to the plug-in library.
#include <clap/clap.h>

bool notecraft_entry_init(const char* path);
void notecraft_entry_deinit();
const void* notecraft_entry_get_factory(const char* id);

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    notecraft_entry_init,
    notecraft_entry_deinit,
    notecraft_entry_get_factory,
};
