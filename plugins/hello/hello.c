/*
 * The smallest possible Ccratch plugin. Copy this to start a new one.
 * Built into build/examples/plugins, where Ccratch does not look: copy it to
 * ~/.local/share/ccratch/plugins to see it in Settings > Plugins.
 */
#include "ccratch/ccr_plugin.h"

#include <stddef.h>

static const ccr_host_api *g_host;

static bool hello_init(const ccr_host_api *host)
{
    g_host = host;
    g_host->log(CCR_LOG_INFO, "hello", "Plugin loaded. Donations appreciated.");
    return true;
}

static void hello_shutdown(void)
{
    g_host = NULL;
}

static const ccr_plugin g_plugin = {
    .abi_version = CCR_PLUGIN_ABI_VERSION,
    .struct_size = sizeof(ccr_plugin),
    .kinds       = CCR_PLUGIN_GENERIC,
    .name        = "hello",
    .version     = "0.0.1",
    .description = "Template plugin that says hi",
    .init        = hello_init,
    .shutdown    = hello_shutdown,
};

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void);

CCR_PLUGIN_EXPORT const ccr_plugin *ccr_plugin_entry(void)
{
    return &g_plugin;
}
