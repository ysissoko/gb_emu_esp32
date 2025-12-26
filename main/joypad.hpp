#pragma once

#include "memory_bus.hpp"

#include "driver/gpio.h"
#include "esp_timer.h"

#include <cstdint>

namespace controller
{
    constexpr gpio_num_t BTN_RIGHT = GPIO_NUM_25;
    constexpr gpio_num_t BTN_LEFT = GPIO_NUM_26;
    constexpr gpio_num_t BTN_UP = GPIO_NUM_27;
    constexpr gpio_num_t BTN_DOWN = GPIO_NUM_32;

    constexpr gpio_num_t BTN_A = GPIO_NUM_33;
    constexpr gpio_num_t BTN_B = GPIO_NUM_12;
    constexpr gpio_num_t BTN_SELECT = GPIO_NUM_13;
    constexpr gpio_num_t BTN_START = GPIO_NUM_14;

    class Joypad
    {
    public:
        Joypad(memory::MemoryBus& mmu) : mmu(mmu)
        {
            initialize();
        }

        uint8_t read(uint8_t joyp_select)
        {
            uint8_t res = joyp_select | 0xC0; // bits 7-6 to HIGH (joypad selection multiplexer)
            res |= 0x0F;                      // bits set to HIGH (relâchés)

            // Directions selected (P14 = 0)
            if (!(joyp_select & (1 << 4)))
            {
                if (pressed(BTN_RIGHT))
                    res &= ~(1 << 0);
                if (pressed(BTN_LEFT))
                    res &= ~(1 << 1);
                if (pressed(BTN_UP))
                    res &= ~(1 << 2);
                if (pressed(BTN_DOWN))
                    res &= ~(1 << 3);
            }

            // Other buttons (A, B, START, SELECT) (P15 = 0)
            if (!(joyp_select & (1 << 5)))
            {
                if (pressed(BTN_A))
                    res &= ~(1 << 0);
                if (pressed(BTN_B))
                    res &= ~(1 << 1);
                if (pressed(BTN_SELECT))
                    res &= ~(1 << 2);
                if (pressed(BTN_START))
                    res &= ~(1 << 3);
            }

            return res;
        }

    private:
    
        void initialize()
        {
            gpio_config_t cfg{};
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            cfg.intr_type = GPIO_INTR_DISABLE;
            cfg.pin_bit_mask =
                (1ULL << BTN_RIGHT) |
                (1ULL << BTN_LEFT) |
                (1ULL << BTN_UP) |
                (1ULL << BTN_DOWN) |
                (1ULL << BTN_A) |
                (1ULL << BTN_B) |
                (1ULL << BTN_SELECT) |
                (1ULL << BTN_START);

            gpio_config(&cfg);

            for (int i = 0; i < 8; i++)
            {
                last_press_time[i] = 0;
            }
        }

        inline uint8_t get_button_index(gpio_num_t pin) const
        {
            if (pin == BTN_RIGHT) return 0;
            if (pin == BTN_LEFT) return 1;
            if (pin == BTN_UP) return 2;
            if (pin == BTN_DOWN) return 3;
            if (pin == BTN_A) return 4;
            if (pin == BTN_B) return 5;
            if (pin == BTN_SELECT) return 6;
            if (pin == BTN_START) return 7;
            return 0;
        }

        inline bool pressed(gpio_num_t pin)
        {
            uint8_t idx = get_button_index(pin);

            // Verify button state (release or press)
            if (gpio_get_level(pin) == 0)
            {
                int64_t now = esp_timer_get_time() / 1000; // Convertir µs en ms
                int64_t last_time = last_press_time[idx];

                // Debouncing: accepter seulement si 20ms se sont écoulées
                if (now - last_time > 20)
                {
                    last_press_time[idx] = now;
                    return true;
                }

                // Bouton pressé mais dans la période de debounce
                // Retourner l'état précédent (pressé)
                return (now - last_time <= 100); // Rester pressé jusqu'à 100ms max
            }

            return false; // Bouton relâché
        }

        // Array to store the latest press time of button matrix
        int64_t last_press_time[8];
        uint8_t prev_buttons_state{0x0F}; // all buttons are released by default
    };
}
