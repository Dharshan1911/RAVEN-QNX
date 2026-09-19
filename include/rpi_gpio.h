#ifndef RPI_GPIO_H
#define RPI_GPIO_H

#include <stdint.h>
#include <sys/mman.h>
#include <hw/inout.h>
#include <stdio.h>

// BCM2711 peripheral base for Pi 4
#define BCM2711_PERIPH_BASE 0xFE000000
#define GPIO_BASE (BCM2711_PERIPH_BASE + 0x200000)
#define GPIO_LEN  0xF4

// Offsets
#define GPFSEL0   0x00
#define GPFSEL1   0x04
#define GPFSEL2   0x08
#define GPSET0    0x1C
#define GPCLR0    0x28
#define GPLEV0    0x34

typedef struct {
    uintptr_t ptr;
} rpi_gpio_t;

static inline int rpi_gpio_init(rpi_gpio_t *gpio) {
    gpio->ptr = mmap_device_io(GPIO_LEN, GPIO_BASE);
    if (gpio->ptr == MAP_DEVICE_FAILED) {
        perror("mmap_device_io");
        return -1;
    }
    return 0;
}

static inline void rpi_gpio_set_input(rpi_gpio_t *gpio, int pin) {
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    uint32_t val = in32(gpio->ptr + GPFSEL0 + (reg * 4));
    val &= ~(7 << shift);
    out32(gpio->ptr + GPFSEL0 + (reg * 4), val);
}

static inline void rpi_gpio_set_output(rpi_gpio_t *gpio, int pin) {
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    uint32_t val = in32(gpio->ptr + GPFSEL0 + (reg * 4));
    val &= ~(7 << shift);
    val |= (1 << shift);
    out32(gpio->ptr + GPFSEL0 + (reg * 4), val);
}

static inline void rpi_gpio_write(rpi_gpio_t *gpio, int pin, int value) {
    if (value) {
        out32(gpio->ptr + GPSET0, 1 << pin);
    } else {
        out32(gpio->ptr + GPCLR0, 1 << pin);
    }
}

static inline int rpi_gpio_read(rpi_gpio_t *gpio, int pin) {
    uint32_t val = in32(gpio->ptr + GPLEV0);
    return (val & (1 << pin)) ? 1 : 0;
}

#endif
