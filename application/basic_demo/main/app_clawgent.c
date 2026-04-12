/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_clawgent.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "basic_demo_lua_modules.h"
#include "basic_demo_wifi.h"
#include "cap_cli.h"
#include "cap_files.h"
#include "cap_im_feishu.h"
#include "cap_im_qq.h"
#include "cap_im_tg.h"
#include "cap_im_wechat.h"
#include "cap_llm_inspect.h"
#include "cap_lua.h"
#include "cap_mcp_client.h"
#include "cap_mcp_server.h"
#include "cap_router_mgr.h"
#include "cap_session_mng.h"
#include "cap_scheduler.h"
#include "cap_skill.h"
#include "cap_time.h"
#include "cap_web_search.h"
#include "claw_event_router.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "app_clawgent";
static const char *const BASIC_DEMO_LLM_VISIBLE_GROUPS[] = {
    "cap_cli",
    "cap_files",
    "cap_router_mgr",
    "cap_skill",
};

#define BASIC_DEMO_MEMORY_SESSION_ROOT   "/fatfs/data/sessions"
#define BASIC_DEMO_MEMORY_LONG_TERM_PATH "/fatfs/data/memory/MEMORY.md"
#define BASIC_DEMO_SKILLS_ROOT_DIR       "/fatfs/data/skills"
#define BASIC_DEMO_LUA_ROOT_DIR       "/fatfs/data/lua"
#define BASIC_DEMO_FATFS_BASE_PATH       "/fatfs/data"
#define BASIC_DEMO_AUTOMATION_RULES_PATH "/fatfs/data/automation/automations.json"
#define BASIC_DEMO_SCHEDULER_RULES_PATH  "/fatfs/data/scheduler/schedules.json"
#define BASIC_DEMO_SCHEDULER_STATE_PATH  "/fatfs/data/scheduler/scheduler_state.json"
#define BASIC_DEMO_IM_ATTACHMENT_ROOT    "/fatfs/data/inbox"
#define BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES (2 * 1024 * 1024)

#define BASIC_DEMO_SYSTEM_PROMPT \
    "You are the clawgent running on ESP32. " \
    "Answer briefly and plainly. " \
    "Treat Skills List as a catalog of optional skills, not as callable cap. " \
    "Use 'activate_skill' to load a skill,and you will gain more callable capabilities\n" \
    "Skills are user-facing functions, while Capabilities are internal functions used by the model.\n" \
    "After completing the task, call 'deactivete_skill' to keep the context streamlined and efficient." \
    "When communicating with the user, refer to skills instead of Capabilities." \

esp_err_t basic_demo_cli_start(void);

static bool basic_demo_time_network_ready(void *ctx)
{
    (void)ctx;
    return basic_demo_wifi_is_connected();
}

static void basic_demo_time_sync_success(bool had_valid_time, void *ctx)
{
    (void)ctx;

    if (!had_valid_time) {
        esp_err_t err = cap_scheduler_handle_time_sync();

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Scheduler rebase after first time sync failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Scheduler rebased after first successful time sync");
        }
    }
}

static esp_err_t init_memory(void)
{
    claw_memory_config_t memory_config = {
        .session_root_dir = BASIC_DEMO_MEMORY_SESSION_ROOT,
        .long_term_memory_path = BASIC_DEMO_MEMORY_LONG_TERM_PATH,
        .max_session_messages = 20,
        .max_message_chars = 1024,
    };
    esp_err_t err;

    err = claw_memory_init(&memory_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init claw_memory: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t init_skills(void)
{
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t) {
        .skills_root_dir = BASIC_DEMO_SKILLS_ROOT_DIR,
        .session_state_root_dir = BASIC_DEMO_MEMORY_SESSION_ROOT,
    }),
    TAG,
    "Failed to init claw_skill");
    return ESP_OK;
}

static esp_err_t init_capabilities(const basic_demo_settings_t *settings)
{
    claw_cap_config_t cap_config = {
        .max_capabilities = 48,
        .max_groups = 20,
    };

    ESP_RETURN_ON_ERROR(claw_cap_init(&cap_config), TAG, "Failed to init claw_cap");

    ESP_RETURN_ON_ERROR(cap_time_set_timezone(settings->time_timezone),
                        TAG,
                        "Failed to set time cap timezone");
    ESP_RETURN_ON_ERROR(cap_files_set_base_dir(BASIC_DEMO_FATFS_BASE_PATH),
                        TAG,
                        "Failed to set files cap base dir");
    ESP_RETURN_ON_ERROR(cap_lua_set_base_dir(BASIC_DEMO_LUA_ROOT_DIR),
                        TAG,
                        "Failed to set Lua base dir");
    ESP_RETURN_ON_ERROR(cap_im_qq_set_attachment_config(
    &(cap_im_qq_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set QQ attachment config");
    ESP_RETURN_ON_ERROR(cap_im_tg_set_attachment_config(
    &(cap_im_tg_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set Telegram attachment config");
    ESP_RETURN_ON_ERROR(cap_im_wechat_set_attachment_config(
    &(cap_im_wechat_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set WeChat attachment config");
    ESP_RETURN_ON_ERROR(cap_im_feishu_set_attachment_config(
    &(cap_im_feishu_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set Feishu attachment config");

    if (settings->qq_app_id[0] && settings->qq_app_secret[0]) {
        ESP_RETURN_ON_ERROR(cap_im_qq_set_credentials(settings->qq_app_id,
                                                      settings->qq_app_secret),
                            TAG,
                            "Failed to set QQ credentials");
    }

    if (settings->feishu_app_id[0] && settings->feishu_app_secret[0]) {
        ESP_RETURN_ON_ERROR(cap_im_feishu_set_credentials(settings->feishu_app_id,
                                                          settings->feishu_app_secret),
                            TAG,
                            "Failed to set Feishu credentials");
    }

    if (settings->tg_bot_token[0]) {
        ESP_RETURN_ON_ERROR(cap_im_tg_set_token(settings->tg_bot_token),
                            TAG,
                            "Failed to set Telegram bot token");
    }

    if (settings->wechat_token[0] && settings->wechat_base_url[0]) {
        ESP_RETURN_ON_ERROR(cap_im_wechat_set_client_config(
        &(cap_im_wechat_client_config_t) {
            .token = settings->wechat_token,
            .base_url = settings->wechat_base_url,
            .cdn_base_url = settings->wechat_cdn_base_url,
            .account_id = settings->wechat_account_id,
        }),
        TAG,
        "Failed to set WeChat client config");
    }

    if (settings->search_brave_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_brave_key(settings->search_brave_key),
                            TAG,
                            "Failed to set Brave search key");
    }

    if (settings->search_tavily_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_tavily_key(settings->search_tavily_key),
                            TAG,
                            "Failed to set Tavily search key");
    }

    ESP_RETURN_ON_ERROR(cap_im_qq_register_group(), TAG, "Failed to register QQ cap");
    ESP_RETURN_ON_ERROR(cap_im_feishu_register_group(),
                        TAG,
                        "Failed to register Feishu cap");
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(),
                        TAG,
                        "Failed to register Telegram cap");
    ESP_RETURN_ON_ERROR(cap_im_wechat_register_group(),
                        TAG,
                        "Failed to register WeChat cap");
    ESP_RETURN_ON_ERROR(cap_files_register_group(), TAG, "Failed to register files cap");
    ESP_RETURN_ON_ERROR(basic_demo_lua_modules_register(),
                        TAG,
                        "Failed to register app Lua modules");
    ESP_RETURN_ON_ERROR(cap_scheduler_register_group(),
                        TAG,
                        "Failed to register scheduler cap");
    ESP_RETURN_ON_ERROR(cap_lua_register_group(), TAG, "Failed to register Lua cap");
    ESP_RETURN_ON_ERROR(cap_mcp_client_register_group(),
                        TAG,
                        "Failed to register MCP client cap");
    ESP_RETURN_ON_ERROR(cap_mcp_server_register_group(),
                        TAG,
                        "Failed to register MCP server cap");
    ESP_RETURN_ON_ERROR(cap_skill_register_group(),
                        TAG,
                        "Failed to register skill cap");
    ESP_RETURN_ON_ERROR(cap_time_register_group(), TAG, "Failed to register time cap");
    ESP_RETURN_ON_ERROR(cap_llm_inspect_register_group(),
                        TAG,
                        "Failed to register LLM inspect cap");
    ESP_RETURN_ON_ERROR(cap_web_search_register_group(),
                        TAG,
                        "Failed to register web search cap");
    ESP_RETURN_ON_ERROR(cap_session_mng_register_group(),
                        TAG,
                        "Failed to register session manager cap");
    ESP_RETURN_ON_ERROR(claw_cap_set_llm_visible_groups(
                            BASIC_DEMO_LLM_VISIBLE_GROUPS,
                            sizeof(BASIC_DEMO_LLM_VISIBLE_GROUPS) /
                            sizeof(BASIC_DEMO_LLM_VISIBLE_GROUPS[0])),
                        TAG,
                        "Failed to set LLM-visible capability groups");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "Failed to start capabilities");

    return ESP_OK;
}

static const char *basic_demo_llm_provider_name(const basic_demo_settings_t *settings)
{
    if (!settings) {
        return "unknown";
    }

    if ((settings->llm_backend_type[0] && strcmp(settings->llm_backend_type, "anthropic") == 0) ||
            (settings->llm_profile[0] && strcmp(settings->llm_profile, "anthropic") == 0)) {
        return "Anthropic";
    }
    if (settings->llm_profile[0] &&
            (strcmp(settings->llm_profile, "qwen") == 0 ||
             strcmp(settings->llm_profile, "qwen_compatible") == 0)) {
        return "Qwen Compatible";
    }
    if (settings->llm_profile[0] && strcmp(settings->llm_profile, "openai") == 0) {
        return "OpenAI";
    }
    return "Custom";
}

static bool basic_demo_llm_is_configured(const basic_demo_settings_t *settings)
{
    return settings &&
           settings->llm_api_key[0] &&
           settings->llm_model[0] &&
           settings->llm_profile[0];
}

esp_err_t app_clawgent_start(const basic_demo_settings_t *settings)
{
    claw_core_config_t core_config = {0};
    claw_event_router_config_t router_config = {
        .rules_path = BASIC_DEMO_AUTOMATION_RULES_PATH,
        .task_stack_size = 6 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 130000,
        .default_route_messages_to_agent = false,
        .session_builder = cap_session_mng_build_session_id,
    };
    bool llm_enabled = false;

    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    llm_enabled = basic_demo_llm_is_configured(settings);
    router_config.default_route_messages_to_agent = llm_enabled;

    ESP_RETURN_ON_ERROR(cap_session_mng_set_session_root_dir(BASIC_DEMO_MEMORY_SESSION_ROOT),
                        TAG,
                        "Failed to configure session manager");
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config),
                        TAG,
                        "Failed to init event router");
    ESP_RETURN_ON_ERROR(cap_scheduler_init(&(cap_scheduler_config_t) {
        .schedules_path = BASIC_DEMO_SCHEDULER_RULES_PATH,
        .state_path = BASIC_DEMO_SCHEDULER_STATE_PATH,
        .default_timezone = settings->time_timezone,
        .tick_ms = 1000,
        .max_items = 32,
        .task_stack_size = 6144,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .publish_event = claw_event_router_publish,
        .persist_after_fire = true,
    }),
    TAG,
    "Failed to init scheduler");
    ESP_RETURN_ON_ERROR(init_memory(), TAG, "Failed to init memory");
    ESP_RETURN_ON_ERROR(init_skills(), TAG, "Failed to init skills");
    ESP_RETURN_ON_ERROR(init_capabilities(settings), TAG, "Failed to init capabilities");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("qq", "qq_send_message"),
                        TAG,
                        "Failed to bind QQ outbound");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("feishu", "feishu_send_message"),
                        TAG,
                        "Failed to bind Feishu outbound");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"),
                        TAG,
                        "Failed to bind Telegram outbound");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("wechat", "wechat_send_message"),
                        TAG,
                        "Failed to bind WeChat outbound");

    core_config.api_key = settings->llm_api_key;
    core_config.backend_type = settings->llm_backend_type;
    core_config.profile = settings->llm_profile;
    core_config.model = settings->llm_model;
    core_config.base_url = settings->llm_base_url;
    core_config.auth_type = settings->llm_auth_type;
    core_config.timeout_ms = (uint32_t)strtoul(settings->llm_timeout_ms, NULL, 10);
    core_config.system_prompt = BASIC_DEMO_SYSTEM_PROMPT;
    core_config.append_session_turn = claw_memory_append_session_turn_callback;
    core_config.call_cap = claw_cap_call_from_core;
    core_config.task_stack_size = 6 * 1024;
    core_config.task_priority = 5;
    core_config.task_core = tskNO_AFFINITY;
    core_config.max_tool_iterations = 10;
    core_config.request_queue_len = 4;
    core_config.response_queue_len = 4;
    core_config.max_context_providers = 5;

    if (!llm_enabled) {
        ESP_LOGW(TAG,
                 "LLM is not fully configured. Provider=%s profile=%s model=%s. "
                 "The demo will start without claw_core; ask, auto-route-to-agent, and image analysis stay disabled until LLM API key, profile, and model are set.",
                 basic_demo_llm_provider_name(settings),
                 settings->llm_profile[0] ? settings->llm_profile : "(empty)",
                 settings->llm_model[0] ? settings->llm_model : "(empty)");
    } else {
        ESP_LOGI(TAG,
                 "Starting LLM provider=%s profile=%s backend=%s model=%s",
                 basic_demo_llm_provider_name(settings),
                 settings->llm_profile,
                 settings->llm_backend_type[0] ? settings->llm_backend_type : "(default)",
                 settings->llm_model);
        ESP_RETURN_ON_ERROR(claw_core_init(&core_config), TAG, "Failed to init claw_core");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_provider),
                            TAG,
                            "Failed to add long-term memory provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_session_history_provider),
                            TAG,
                            "Failed to add session history provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_skills_list_provider),
                            TAG,
                            "Failed to add skills list provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_active_skill_docs_provider),
                            TAG,
                            "Failed to add active skill docs provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_cap_tools_provider),
                            TAG,
                            "Failed to add cap tools provider");
        ESP_RETURN_ON_ERROR(claw_core_start(), TAG, "Failed to start claw_core");
    }

    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
    ESP_RETURN_ON_ERROR(cap_time_sync_service_start(&(cap_time_sync_service_config_t) {
        .network_ready = basic_demo_time_network_ready,
        .on_sync_success = basic_demo_time_sync_success,
    }),
    TAG,
    "Failed to start time sync service");
    ESP_RETURN_ON_ERROR(cap_scheduler_start(), TAG, "Failed to start scheduler");
    ESP_RETURN_ON_ERROR(basic_demo_cli_start(), TAG, "Failed to start CLI");

    return ESP_OK;
}
