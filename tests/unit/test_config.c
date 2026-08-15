/* test_config.c - OPENROUTER_API_KEY fallback behavior */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../test_util.h"
#include "../../src/cli/cmd/config.h"

static void test_openrouter_fallback(void) {
    /* clean env */
    unsetenv("PUCI_API_KEY");
    unsetenv("PUCI_BASE_URL");
    unsetenv("PUCI_MODEL");
    unsetenv("OPENROUTER_API_KEY");
    setenv("OPENROUTER_API_KEY", "sk-or-v1-test123", 1);

    puci_provider_config *cfg = puci_cli_provider("openai");
    CHECK(cfg != NULL);
    CHECK(cfg->api_key && strcmp(cfg->api_key, "sk-or-v1-test123") == 0);
    CHECK(cfg->base_url && strcmp(cfg->base_url, "https://openrouter.ai/api/v1") == 0);
    CHECK(cfg->model && strcmp(cfg->model, "anthropic/claude-3.5-sonnet") == 0);

    /* PUCI_API_KEY takes precedence */
    setenv("PUCI_API_KEY", "sk-puci-direct", 1);
    cfg = puci_cli_provider("openai");
    CHECK(cfg->api_key && strcmp(cfg->api_key, "sk-puci-direct") == 0);

    /* PUCI_BASE_URL explicit should not be overridden by fallback */
    setenv("PUCI_BASE_URL", "https://api.openai.com/v1", 1);
    unsetenv("PUCI_API_KEY");
    setenv("OPENROUTER_API_KEY", "sk-or-v1-test123", 1);
    cfg = puci_cli_provider("openai");
    CHECK(strcmp(cfg->base_url, "https://api.openai.com/v1") == 0);

    /* cleanup */
    unsetenv("PUCI_API_KEY");
    unsetenv("PUCI_BASE_URL");
    unsetenv("PUCI_MODEL");
    unsetenv("OPENROUTER_API_KEY");
}

static void test_no_fallback_when_no_keys(void) {
    unsetenv("PUCI_API_KEY");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("PUCI_BASE_URL");
    unsetenv("PUCI_MODEL");
    /* Isolate from real global config (~/.config/puci/config.toml) which may contain OpenRouter defaults */
    char tmpl[] = "/tmp/puci-test-empty-XXXXXX";
    char *tmpdir = mkdtemp(tmpl);
    const char *old_xdg = getenv("XDG_CONFIG_HOME");
    char *old_dup = old_xdg ? strdup(old_xdg) : NULL;
    if (tmpdir) setenv("XDG_CONFIG_HOME", tmpdir, 1);
    puci_provider_config *cfg = puci_cli_provider("openai");
    CHECK(cfg != NULL);
    CHECK(cfg->api_key == NULL);
    CHECK(strcmp(cfg->base_url, "https://api.openai.com/v1") == 0);
    CHECK(strcmp(cfg->model, "gpt-4o-mini") == 0);
    if (old_dup) setenv("XDG_CONFIG_HOME", old_dup, 1); else unsetenv("XDG_CONFIG_HOME");
    free(old_dup);
    if (tmpdir) rmdir(tmpdir);
}

int main(void) {
    /* Isolate both tests from real global config for determinism */
    char tmpl2[] = "/tmp/puci-test-cfg-XXXXXX";
    char *tmpdir2 = mkdtemp(tmpl2);
    const char *old_xdg2 = getenv("XDG_CONFIG_HOME");
    char *old_dup2 = old_xdg2 ? strdup(old_xdg2) : NULL;
    if (tmpdir2) setenv("XDG_CONFIG_HOME", tmpdir2, 1);
    test_openrouter_fallback();
    if (old_dup2) setenv("XDG_CONFIG_HOME", old_dup2, 1); else unsetenv("XDG_CONFIG_HOME");
    free(old_dup2);
    if (tmpdir2) rmdir(tmpdir2);
    test_no_fallback_when_no_keys();
    TEST_END();
}
