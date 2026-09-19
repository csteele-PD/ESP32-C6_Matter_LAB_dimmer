#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_providers.h>
#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "matter_c6_lab";

static constexpr gpio_num_t FACTORY_RESET_GPIO = GPIO_NUM_9;
static constexpr uint32_t MATTER_SETUP_PIN = 20202021;
static constexpr uint16_t MATTER_SETUP_DISCRIMINATOR = 3840;
static constexpr char MATTER_VENDOR_NAME[] = "ACLYS_LAB";
static constexpr char MATTER_PRODUCT_NAME[] = "MatterC6Lab";
static constexpr char MATTER_NODE_LABEL[] = "C6 Thread Lab";

static uint16_t lab_endpoint_id;
static bool internal_update;

class LabDeviceInstanceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    CHIP_ERROR GetVendorName(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, MATTER_VENDOR_NAME);
    }

    CHIP_ERROR GetVendorId(uint16_t &vendor_id) override
    {
        vendor_id = CONFIG_DEVICE_VENDOR_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductName(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, MATTER_PRODUCT_NAME);
    }

    CHIP_ERROR GetProductId(uint16_t &product_id) override
    {
        product_id = CONFIG_DEVICE_PRODUCT_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPartNumber(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetProductURL(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetProductLabel(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSerialNumber(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, "matter-c6-lab-001");
    }

    CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override
    {
        (void)year;
        (void)month;
        (void)day;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetHardwareVersion(uint16_t &hardware_version) override
    {
        hardware_version = 1;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersionString(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, "ESP32-C6FH8");
    }

    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &unique_id_span) override
    {
        static constexpr uint8_t unique_id[] = {
            0x41, 0x43, 0x4C, 0x59, 0x53, 0x2D, 0x43, 0x36,
            0x2D, 0x4C, 0x41, 0x42, 0x2D, 0x30, 0x30, 0x31,
        };

        if (unique_id_span.size() < sizeof(unique_id)) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }

        memcpy(unique_id_span.data(), unique_id, sizeof(unique_id));
        unique_id_span.reduce_size(sizeof(unique_id));
        return CHIP_NO_ERROR;
    }

private:
    static CHIP_ERROR copy_string(char *buf, size_t buf_size, const char *value)
    {
        if (buf_size < strlen(value) + 1) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }

        chip::Platform::CopyString(buf, buf_size, value);
        return CHIP_NO_ERROR;
    }
};

static LabDeviceInstanceInfoProvider lab_device_instance_info_provider;

static void maybe_factory_reset()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << FACTORY_RESET_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(FACTORY_RESET_GPIO) != 0) {
        return;
    }

    ESP_LOGW(TAG, "BOOT/GPIO9 held low; erasing NVS and restarting");
    ESP_ERROR_CHECK(nvs_flash_erase());
    esp_restart();
}

static void log_manual_pairing_code()
{
    chip::PayloadContents payload;
    payload.version = 0;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.discriminator.SetLongValue(MATTER_SETUP_DISCRIMINATOR);
    payload.setUpPINCode = MATTER_SETUP_PIN;

    char code[chip::kManualSetupLongCodeCharLength + 2] = {};
    chip::MutableCharSpan span(code, sizeof(code));
    CHIP_ERROR err = chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(span);
    if (err == CHIP_NO_ERROR) {
        code[span.size()] = '\0';
        ESP_LOGI(TAG, "Matter manual pairing code=%s", code);
    } else {
        ESP_LOGW(TAG, "Matter manual pairing code generation failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static esp_err_t configure_openthread_platform()
{
    esp_openthread_platform_config_t config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size = 10,
        },
    };

    ESP_LOGI(TAG, "OpenThread platform radio=NATIVE storage=nvs");
    ESP_RETURN_ON_ERROR(set_openthread_platform_config(&config), TAG, "set OpenThread platform config failed");
    return ESP_OK;
}

static esp_err_t matter_attribute_callback(attribute::callback_type_t type,
                                           uint16_t endpoint_id,
                                           uint32_t cluster_id,
                                           uint32_t attribute_id,
                                           esp_matter_attr_val_t *val,
                                           void *priv_data)
{
    (void)priv_data;

    if ((type != attribute::PRE_UPDATE && type != attribute::POST_UPDATE) ||
        endpoint_id != lab_endpoint_id ||
        val == nullptr ||
        internal_update) {
        return ESP_OK;
    }

    const char *phase = type == attribute::PRE_UPDATE ? "PRE_UPDATE" : "POST_UPDATE";
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        ESP_LOGI(TAG, "Matter OnOff %s value=%s", phase, val->val.b ? "on" : "off");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        ESP_LOGI(TAG, "Matter LevelControl %s current_level=%u", phase, val->val.u8);
    }
    return ESP_OK;
}

static void matter_event_callback(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    if (event == nullptr) {
        return;
    }

    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "commissioning window opened");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "commissioning session stopped");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "fabric committed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "fabric removed");
        break;
    default:
        break;
    }
}

static void log_memory(const char *phase)
{
    ESP_LOGI(TAG,
             "heap %s internal_free=%u internal_largest=%u default_free=%u default_largest=%u chip_stack=%u",
             phase,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             CONFIG_CHIP_TASK_STACK_SIZE);
}

static esp_err_t start_matter()
{
    node::config_t node_config;
    strlcpy(node_config.root_node.basic_information.node_label,
            MATTER_NODE_LABEL,
            sizeof(node_config.root_node.basic_information.node_label));

    esp_matter::set_custom_device_instance_info_provider(&lab_device_instance_info_provider);

    node_t *node = node::create(&node_config, matter_attribute_callback, nullptr);
    ESP_RETURN_ON_FALSE(node != nullptr, ESP_FAIL, TAG, "Matter node create failed");

    endpoint::dimmable_light::config_t endpoint_config;
    endpoint_config.on_off.on_off = false;
    endpoint_config.level_control.current_level = nullable<uint8_t>(128);
    endpoint_t *endpoint = endpoint::dimmable_light::create(node, &endpoint_config, ENDPOINT_FLAG_NONE, nullptr);
    ESP_RETURN_ON_FALSE(endpoint != nullptr, ESP_FAIL, TAG, "Matter Dimmable Light endpoint create failed");

    lab_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG,
             "Matter identity manufacturer=%s model=%s vendor_id=0x%04X product_id=0x%04X",
             MATTER_VENDOR_NAME,
             MATTER_PRODUCT_NAME,
             CONFIG_DEVICE_VENDOR_ID,
             CONFIG_DEVICE_PRODUCT_ID);
    ESP_LOGI(TAG,
             "starting Matter-over-Thread Dimmable Light endpoint=%u setup_pin=%" PRIu32 " discriminator=%u",
             lab_endpoint_id,
             MATTER_SETUP_PIN,
             MATTER_SETUP_DISCRIMINATOR);
    log_manual_pairing_code();
    log_memory("before Matter start");

    ESP_RETURN_ON_ERROR(esp_matter::start(matter_event_callback), TAG, "Matter start failed");

    const size_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    ESP_LOGI(TAG, "Matter started fabric_count=%u", static_cast<unsigned>(fabric_count));
    log_memory("after Matter start");
    return ESP_OK;
}

extern "C" void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Starting ESP32-C6 Matter-over-Thread lab version=%s", app_desc->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    maybe_factory_reset();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(configure_openthread_platform());
    ESP_ERROR_CHECK(start_matter());
}
