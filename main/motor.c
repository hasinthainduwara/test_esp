#include "motor.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "motor";

/* LEDC_CHANNEL_0 generates the camera XCLK (esp32-camera xclk.c) — motors must
 * not touch it or the camera clock dies. Motors use channels 1-4. */
#define MOTOR_CH_LEFT_RPWM  LEDC_CHANNEL_1
#define MOTOR_CH_LEFT_LPWM  LEDC_CHANNEL_2
#define MOTOR_CH_RIGHT_RPWM LEDC_CHANNEL_3
#define MOTOR_CH_RIGHT_LPWM LEDC_CHANNEL_4

static bool s_estop = false;
static bool s_motors_ready = false;

#if CONFIG_SPIRAM && CONFIG_SPIRAM_MODE_OCT
static bool gpio_conflicts_octal_psram(int gpio)
{
    return gpio >= 35 && gpio <= 38;
}

static bool motor_pins_conflict_psram(void)
{
    const int pins[] = {
        CONFIG_MOTOR_LEFT_RPWM_GPIO, CONFIG_MOTOR_LEFT_LPWM_GPIO,
        CONFIG_MOTOR_LEFT_R_EN_GPIO, CONFIG_MOTOR_LEFT_L_EN_GPIO,
        CONFIG_MOTOR_RIGHT_RPWM_GPIO, CONFIG_MOTOR_RIGHT_LPWM_GPIO,
        CONFIG_MOTOR_RIGHT_R_EN_GPIO, CONFIG_MOTOR_RIGHT_L_EN_GPIO,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (gpio_conflicts_octal_psram(pins[i])) {
            return true;
        }
    }
    return false;
}
#else
static bool motor_pins_conflict_psram(void)
{
    return false;
}
#endif

static void set_pwm(ledc_channel_t channel, uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static void left_motor(int speed)
{
    if (speed > 0) {
        set_pwm(MOTOR_CH_LEFT_RPWM, speed);
        set_pwm(MOTOR_CH_LEFT_LPWM, 0);
    } else if (speed < 0) {
        set_pwm(MOTOR_CH_LEFT_RPWM, 0);
        set_pwm(MOTOR_CH_LEFT_LPWM, -speed);
    } else {
        set_pwm(MOTOR_CH_LEFT_RPWM, 0);
        set_pwm(MOTOR_CH_LEFT_LPWM, 0);
    }
}

static void right_motor(int speed)
{
    if (speed > 0) {
        set_pwm(MOTOR_CH_RIGHT_RPWM, speed);
        set_pwm(MOTOR_CH_RIGHT_LPWM, 0);
    } else if (speed < 0) {
        set_pwm(MOTOR_CH_RIGHT_RPWM, 0);
        set_pwm(MOTOR_CH_RIGHT_LPWM, -speed);
    } else {
        set_pwm(MOTOR_CH_RIGHT_RPWM, 0);
        set_pwm(MOTOR_CH_RIGHT_LPWM, 0);
    }
}

static void set_side_enable(int r_en_gpio, int l_en_gpio, bool enabled)
{
    const int level = enabled ? 1 : 0;
    gpio_set_level(r_en_gpio, level);
    gpio_set_level(l_en_gpio, level);
}

static void set_driver_enable(bool enabled)
{
    set_side_enable(CONFIG_MOTOR_LEFT_R_EN_GPIO, CONFIG_MOTOR_LEFT_L_EN_GPIO, enabled);
    set_side_enable(CONFIG_MOTOR_RIGHT_R_EN_GPIO, CONFIG_MOTOR_RIGHT_L_EN_GPIO, enabled);
}

esp_err_t motor_init(void)
{
    if (!CONFIG_MOTOR_ENABLE) {
        ESP_LOGW(TAG, "Motor control disabled in config (CONFIG_MOTOR_ENABLE=n)");
        return ESP_OK;
    }

    if (motor_pins_conflict_psram()) {
        ESP_LOGE(TAG,
                 "Motor GPIO conflict: GPIO35-38 are octal PSRAM pins on N16R8/N8R8 boards.");
        ESP_LOGE(TAG,
                 "Use Freenove right-side set: L 39/40/41/42, R 47/48/21/45, LED 0.");
        ESP_LOGE(TAG, "Motors disabled — WiFi/camera will run without motor control.");
        return ESP_ERR_INVALID_STATE;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channels[] = {
        {
            .gpio_num = CONFIG_MOTOR_LEFT_RPWM_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = MOTOR_CH_LEFT_RPWM,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        },
        {
            .gpio_num = CONFIG_MOTOR_LEFT_LPWM_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = MOTOR_CH_LEFT_LPWM,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        },
        {
            .gpio_num = CONFIG_MOTOR_RIGHT_RPWM_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = MOTOR_CH_RIGHT_RPWM,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        },
        {
            .gpio_num = CONFIG_MOTOR_RIGHT_LPWM_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = MOTOR_CH_RIGHT_LPWM,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        },
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        err = ledc_channel_config(&channels[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LEDC channel %zu (GPIO %d) failed: %s", i, channels[i].gpio_num,
                     esp_err_to_name(err));
            return err;
        }
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_MOTOR_LEFT_R_EN_GPIO) |
                        (1ULL << CONFIG_MOTOR_LEFT_L_EN_GPIO) |
                        (1ULL << CONFIG_MOTOR_RIGHT_R_EN_GPIO) |
                        (1ULL << CONFIG_MOTOR_RIGHT_L_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Motor enable GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (CONFIG_MOTOR_LED_GPIO >= 0) {
        gpio_config_t led_io = {
            .pin_bit_mask = (1ULL << (unsigned)CONFIG_MOTOR_LED_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&led_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LED GPIO config failed: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level(CONFIG_MOTOR_LED_GPIO, 0);
    }

    set_driver_enable(false);
    motor_stop();
    s_motors_ready = true;

    ESP_LOGI(TAG,
             "Motor drivers ready (L: RPWM=%d LPWM=%d R_EN=%d L_EN=%d | "
             "R: RPWM=%d LPWM=%d R_EN=%d L_EN=%d)",
             CONFIG_MOTOR_LEFT_RPWM_GPIO, CONFIG_MOTOR_LEFT_LPWM_GPIO,
             CONFIG_MOTOR_LEFT_R_EN_GPIO, CONFIG_MOTOR_LEFT_L_EN_GPIO,
             CONFIG_MOTOR_RIGHT_RPWM_GPIO, CONFIG_MOTOR_RIGHT_LPWM_GPIO,
             CONFIG_MOTOR_RIGHT_R_EN_GPIO, CONFIG_MOTOR_RIGHT_L_EN_GPIO);
    return ESP_OK;
}

void motor_stop(void)
{
    left_motor(0);
    right_motor(0);
    set_driver_enable(false);
}

void motor_emergency_stop(void)
{
    s_estop = true;
    motor_stop();
    ESP_LOGW(TAG, "EMERGENCY STOP");
}

void motor_clear_emergency_stop(void)
{
    s_estop = false;
    ESP_LOGI(TAG, "E-stop cleared");
}

bool motor_is_estop(void)
{
    return s_estop;
}

esp_err_t motor_move(const char *direction)
{
    if (!CONFIG_MOTOR_ENABLE || !s_motors_ready) {
        ESP_LOGW(TAG, "Move ignored — motors not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_estop) {
        ESP_LOGW(TAG, "Move ignored — E-stop active");
        return ESP_ERR_INVALID_STATE;
    }
    if (direction == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int speed = CONFIG_MOTOR_PWM_DUTY;

    if (strcmp(direction, "stop") == 0) {
        motor_stop();
        ESP_LOGI(TAG, "Move: stop");
        return ESP_OK;
    }

    set_driver_enable(true);

    /* Forward: left CCW (LPWM), right CW (RPWM). Sides use opposite PWM polarity. */
    if (strcmp(direction, "forward") == 0) {
        left_motor(-speed);
        right_motor(speed);
        ESP_LOGI(TAG, "Move: forward (L=CCW R=CW)");
        return ESP_OK;
    }
    if (strcmp(direction, "reverse") == 0) {
        left_motor(speed);
        right_motor(-speed);
        ESP_LOGI(TAG, "Move: reverse (L=CW R=CCW)");
        return ESP_OK;
    }
    /* Turn: both sides spin the same raw direction, which — because the two
     * motors are mounted mirrored (see forward/reverse above) — rotates the
     * robot in place. Matches the verified convention from
     * Robot_Slave_Firmware/main/motor.c. */
    if (strcmp(direction, "left") == 0) {
        left_motor(-speed);
        right_motor(-speed);
        ESP_LOGI(TAG, "Move: left");
        return ESP_OK;
    }
    if (strcmp(direction, "right") == 0) {
        left_motor(speed);
        right_motor(speed);
        ESP_LOGI(TAG, "Move: right");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unknown direction: %s", direction);
    return ESP_ERR_INVALID_ARG;
}

void motor_led_set(bool on)
{
    if (CONFIG_MOTOR_LED_GPIO < 0) {
        return;
    }
    gpio_set_level(CONFIG_MOTOR_LED_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
}
