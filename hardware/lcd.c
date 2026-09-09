#include "lcd.h"

#include "main.h"
#include "spi.h"

#include <stddef.h>

/* ST7789 HAL port for the STM32 Pocket 1.54 inch 240 x 240 panel. */
#define LCD_CMD_SWRESET 0x01U
#define LCD_CMD_SLPOUT  0x11U
#define LCD_CMD_NORON   0x13U
#define LCD_CMD_INVON   0x21U
#define LCD_CMD_DISPON  0x29U
#define LCD_CMD_CASET   0x2AU
#define LCD_CMD_RASET   0x2BU
#define LCD_CMD_RAMWR   0x2CU
#define LCD_CMD_MADCTL  0x36U
#define LCD_CMD_COLMOD  0x3AU

#define LCD_SPI_TIMEOUT        1000U
#define LCD_COLOR_CHUNK_PIXELS 64U

static void LCD_Select(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

static void LCD_Unselect(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

static void LCD_WriteCommand(uint8_t command)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
    LCD_Select();
    (void)HAL_SPI_Transmit(&hspi2, &command, 1U, LCD_SPI_TIMEOUT);
    LCD_Unselect();
}

static void LCD_WriteData(const uint8_t *data, uint16_t size)
{
    if ((data == NULL) || (size == 0U))
    {
        return;
    }

    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    LCD_Select();
    (void)HAL_SPI_Transmit(&hspi2, (uint8_t *)data, size, LCD_SPI_TIMEOUT);
    LCD_Unselect();
}

static void LCD_WriteCommandData(uint8_t command, const uint8_t *data,
                                 uint16_t size)
{
    LCD_WriteCommand(command);
    LCD_WriteData(data, size);
}

static void LCD_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1,
                                 uint16_t y1)
{
    uint8_t data[4];

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    LCD_WriteCommandData(LCD_CMD_CASET, data, sizeof(data));

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    LCD_WriteCommandData(LCD_CMD_RASET, data, sizeof(data));

    LCD_WriteCommand(LCD_CMD_RAMWR);
}

void LCD_DrawColorColumn(uint16_t x, uint16_t y, const uint16_t *pixels, uint16_t height)
{
    uint8_t data[2U * LCD_HEIGHT];
    if (!pixels || x >= LCD_WIDTH || y >= LCD_HEIGHT || !height) return;
    if (height > LCD_HEIGHT - y) height = LCD_HEIGHT - y;
    for (uint16_t i = 0; i < height; ++i) {
        data[2U*i] = (uint8_t)(pixels[i] >> 8);
        data[2U*i+1U] = (uint8_t)pixels[i];
    }
    LCD_SetAddressWindow(x, y, x, y + height - 1U);
    LCD_WriteData(data, 2U * height);
}

void LCD_SetBacklight(uint8_t enabled)
{
    HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void LCD_Init(void)
{
    static const uint8_t porch[] = {0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U};
    static const uint8_t power[] = {0xA4U, 0xA1U};
    static const uint8_t positive_gamma[] = {
        0xD0U, 0x04U, 0x0DU, 0x11U, 0x13U, 0x2BU, 0x3FU,
        0x54U, 0x4CU, 0x18U, 0x0DU, 0x0BU, 0x1FU, 0x23U};
    static const uint8_t negative_gamma[] = {
        0xD0U, 0x04U, 0x0CU, 0x11U, 0x13U, 0x2CU, 0x3FU,
        0x44U, 0x51U, 0x2FU, 0x1FU, 0x1FU, 0x20U, 0x23U};
    uint8_t value;

    LCD_SetBacklight(0U);
    LCD_Unselect();

    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(20U);
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(120U);

    LCD_WriteCommand(LCD_CMD_SWRESET);
    HAL_Delay(150U);
    LCD_WriteCommand(LCD_CMD_SLPOUT);
    HAL_Delay(120U);

    value = 0x00U; /* portrait, RGB order */
    LCD_WriteCommandData(LCD_CMD_MADCTL, &value, 1U);
    value = 0x55U; /* RGB565, 16 bits per pixel */
    LCD_WriteCommandData(LCD_CMD_COLMOD, &value, 1U);

    LCD_WriteCommandData(0xB2U, porch, sizeof(porch));
    value = 0x35U;
    LCD_WriteCommandData(0xB7U, &value, 1U);
    value = 0x19U;
    LCD_WriteCommandData(0xBBU, &value, 1U);
    value = 0x2CU;
    LCD_WriteCommandData(0xC0U, &value, 1U);
    value = 0x01U;
    LCD_WriteCommandData(0xC2U, &value, 1U);
    value = 0x12U;
    LCD_WriteCommandData(0xC3U, &value, 1U);
    value = 0x20U;
    LCD_WriteCommandData(0xC4U, &value, 1U);
    value = 0x0FU;
    LCD_WriteCommandData(0xC6U, &value, 1U);
    LCD_WriteCommandData(0xD0U, power, sizeof(power));
    LCD_WriteCommandData(0xE0U, positive_gamma, sizeof(positive_gamma));
    LCD_WriteCommandData(0xE1U, negative_gamma, sizeof(negative_gamma));

    LCD_WriteCommand(LCD_CMD_INVON);
    LCD_WriteCommand(LCD_CMD_NORON);
    HAL_Delay(10U);
    LCD_WriteCommand(LCD_CMD_DISPON);
    HAL_Delay(120U);

    LCD_Fill(LCD_COLOR_BLACK);
    LCD_SetBacklight(1U);
}

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                  uint16_t color)
{
    static uint8_t pixels[LCD_COLOR_CHUNK_PIXELS * 2U];
    uint32_t remaining;
    uint16_t chunk;
    uint16_t i;

    if ((width == 0U) || (height == 0U) || (x >= LCD_WIDTH) ||
        (y >= LCD_HEIGHT))
    {
        return;
    }

    if (((uint32_t)x + width) > LCD_WIDTH)
    {
        width = (uint16_t)(LCD_WIDTH - x);
    }
    if (((uint32_t)y + height) > LCD_HEIGHT)
    {
        height = (uint16_t)(LCD_HEIGHT - y);
    }

    for (i = 0U; i < LCD_COLOR_CHUNK_PIXELS; ++i)
    {
        pixels[i * 2U] = (uint8_t)(color >> 8);
        pixels[i * 2U + 1U] = (uint8_t)color;
    }

    LCD_SetAddressWindow(x, y, (uint16_t)(x + width - 1U),
                         (uint16_t)(y + height - 1U));
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    LCD_Select();

    remaining = (uint32_t)width * height;
    while (remaining > 0U)
    {
        chunk = (remaining > LCD_COLOR_CHUNK_PIXELS)
                    ? LCD_COLOR_CHUNK_PIXELS
                    : (uint16_t)remaining;
        (void)HAL_SPI_Transmit(&hspi2, pixels, (uint16_t)(chunk * 2U),
                               LCD_SPI_TIMEOUT);
        remaining -= chunk;
    }

    LCD_Unselect();
}

void LCD_Fill(uint16_t color)
{
    LCD_FillRect(0U, 0U, LCD_WIDTH, LCD_HEIGHT, color);
}

void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_FillRect(x, y, 1U, 1U, color);
}

static const uint8_t *LCD_GetGlyph(char character)
{
    static const uint8_t blank[5] = {0U, 0U, 0U, 0U, 0U};
    static const uint8_t glyph_a[5] = {0x7EU, 0x11U, 0x11U, 0x11U, 0x7EU};
    static const uint8_t glyph_b[5] = {0x7FU, 0x49U, 0x49U, 0x49U, 0x36U};
    static const uint8_t glyph_d[5] = {0x7FU, 0x41U, 0x41U, 0x41U, 0x3EU};
    static const uint8_t glyph_e[5] = {0x7FU, 0x49U, 0x49U, 0x49U, 0x41U};
    static const uint8_t glyph_f[5] = {0x7FU, 0x09U, 0x09U, 0x09U, 0x01U};
    static const uint8_t glyph_g[5] = {0x3EU, 0x41U, 0x49U, 0x49U, 0x7AU};
    static const uint8_t glyph_h[5] = {0x7FU, 0x08U, 0x08U, 0x08U, 0x7FU};
    static const uint8_t glyph_i[5] = {0x00U, 0x41U, 0x7FU, 0x41U, 0x00U};
    static const uint8_t glyph_k[5] = {0x7FU, 0x08U, 0x14U, 0x22U, 0x41U};
    static const uint8_t glyph_l[5] = {0x7FU, 0x40U, 0x40U, 0x40U, 0x40U};
    static const uint8_t glyph_m[5] = {0x7FU, 0x02U, 0x04U, 0x02U, 0x7FU};
    static const uint8_t glyph_n[5] = {0x7FU, 0x04U, 0x08U, 0x10U, 0x7FU};
    static const uint8_t glyph_o[5] = {0x3EU, 0x41U, 0x41U, 0x41U, 0x3EU};
    static const uint8_t glyph_r[5] = {0x7FU, 0x09U, 0x19U, 0x29U, 0x46U};
    static const uint8_t glyph_s[5] = {0x46U, 0x49U, 0x49U, 0x49U, 0x31U};
    static const uint8_t glyph_t[5] = {0x01U, 0x01U, 0x7FU, 0x01U, 0x01U};
    static const uint8_t glyph_w[5] = {0x3FU, 0x40U, 0x38U, 0x40U, 0x3FU};
    static const uint8_t glyph_0[5] = {0x3EU, 0x51U, 0x49U, 0x45U, 0x3EU};
    static const uint8_t glyph_1[5] = {0x00U, 0x42U, 0x7FU, 0x40U, 0x00U};
    static const uint8_t glyph_2[5] = {0x62U, 0x51U, 0x49U, 0x49U, 0x46U};
    static const uint8_t glyph_3[5] = {0x22U, 0x41U, 0x49U, 0x49U, 0x36U};
    static const uint8_t glyph_4[5] = {0x18U, 0x14U, 0x12U, 0x7FU, 0x10U};
    static const uint8_t glyph_5[5] = {0x2FU, 0x49U, 0x49U, 0x49U, 0x31U};
    static const uint8_t glyph_6[5] = {0x3EU, 0x49U, 0x49U, 0x49U, 0x30U};
    static const uint8_t glyph_7[5] = {0x01U, 0x71U, 0x09U, 0x05U, 0x03U};
    static const uint8_t glyph_8[5] = {0x36U, 0x49U, 0x49U, 0x49U, 0x36U};
    static const uint8_t glyph_9[5] = {0x06U, 0x49U, 0x49U, 0x29U, 0x1EU};

    switch (character)
    {
        case 'C': { static const uint8_t g[5] = {0x3E,0x41,0x41,0x41,0x22}; return g; }
        case 'P': { static const uint8_t g[5] = {0x7F,0x09,0x09,0x09,0x06}; return g; }
        case 'U': { static const uint8_t g[5] = {0x3F,0x40,0x40,0x40,0x3F}; return g; }
        case 'V': { static const uint8_t g[5] = {0x1F,0x20,0x40,0x20,0x1F}; return g; }
        case 'Y': { static const uint8_t g[5] = {0x07,0x08,0x70,0x08,0x07}; return g; }
        case 'Z': { static const uint8_t g[5] = {0x61,0x51,0x49,0x45,0x43}; return g; }
        case 'X': { static const uint8_t g[5] = {0x63,0x14,0x08,0x14,0x63}; return g; }
        case '-': { static const uint8_t g[5] = {0x08,0x08,0x08,0x08,0x08}; return g; }
        case '.': { static const uint8_t g[5] = {0,0x60,0x60,0,0}; return g; }
        case 'A': return glyph_a;
        case 'B': return glyph_b;
        case 'D': return glyph_d;
        case 'E': return glyph_e;
        case 'F': return glyph_f;
        case 'G': return glyph_g;
        case 'H': return glyph_h;
        case 'I': return glyph_i;
        case 'K': return glyph_k;
        case 'L': return glyph_l;
        case 'M': return glyph_m;
        case 'N': return glyph_n;
        case 'O': return glyph_o;
        case 'R': return glyph_r;
        case 'S': return glyph_s;
        case 'T': return glyph_t;
        case 'W': return glyph_w;
        case '0': return glyph_0;
        case '1': return glyph_1;
        case '2': return glyph_2;
        case '3': return glyph_3;
        case '4': return glyph_4;
        case '5': return glyph_5;
        case '6': return glyph_6;
        case '7': return glyph_7;
        case '8': return glyph_8;
        case '9': return glyph_9;
        default: return blank;
    }
}

void LCD_UpdateTextField(LCDTextField *field, uint16_t x, uint16_t y,
                         const char *text, uint8_t cells, uint8_t scale,
                         uint16_t foreground, uint16_t background)
{
    /* Compose a complete character (including blank pixels) before writing.
       There is no clear-then-draw black interval and no full-screen buffer. */
    uint8_t pixels[12U * 14U * 2U];
    if (!field || !text || !cells || cells > sizeof(field->text) ||
        !scale || scale > 2U || x + cells * 6U * scale > LCD_WIDTH ||
        y + 7U * scale > LCD_HEIGHT) return;
    uint16_t width = 6U * scale, height = 7U * scale;
    uint8_t color_changed = !field->initialized || field->foreground != foreground ||
                            field->background != background;
    uint8_t ended = 0U;
    for (uint8_t cell = 0U; cell < cells; ++cell) {
        char c = ' ';
        if (!ended) {
            c = text[cell];
            if (!c) { c = ' '; ended = 1U; }
        }
        if (!color_changed && field->text[cell] == c) continue;
        const uint8_t *glyph = LCD_GetGlyph(c);
        for (uint16_t row = 0U; row < height; ++row)
            for (uint16_t col = 0U; col < width; ++col) {
                uint16_t color = (col / scale < 5U &&
                    (glyph[col / scale] & (1U << (row / scale)))) ? foreground : background;
                uint16_t offset = 2U * (row * width + col);
                pixels[offset] = (uint8_t)(color >> 8);
                pixels[offset + 1U] = (uint8_t)color;
            }
        uint16_t left = x + cell * width;
        LCD_SetAddressWindow(left, y, left + width - 1U, y + height - 1U);
        LCD_WriteData(pixels, 2U * width * height);
        field->text[cell] = c;
    }
    field->foreground = foreground;
    field->background = background;
    field->initialized = 1U;
}

void LCD_DrawString(uint16_t x, uint16_t y, const char *text, uint8_t scale,
                    uint16_t color)
{
    const uint8_t *glyph;
    uint8_t column;
    uint8_t row;

    if ((text == NULL) || (scale == 0U))
    {
        return;
    }

    while ((*text != '\0') && (x < LCD_WIDTH))
    {
        glyph = LCD_GetGlyph(*text);
        for (column = 0U; column < 5U; ++column)
        {
            for (row = 0U; row < 7U; ++row)
            {
                if ((glyph[column] & (1U << row)) != 0U)
                {
                    LCD_FillRect((uint16_t)(x + column * scale),
                                 (uint16_t)(y + row * scale), scale, scale,
                                 color);
                }
            }
        }
        x = (uint16_t)(x + 6U * scale);
        ++text;
    }
}

void LCD_DrawUInt16(uint16_t x, uint16_t y, uint16_t value,
                    uint8_t minimum_digits, uint8_t scale, uint16_t color)
{
    char text[6];
    uint8_t length = 0U;
    uint8_t index;
    char digit;

    if (minimum_digits > 5U)
    {
        minimum_digits = 5U;
    }

    do
    {
        text[length] = (char)('0' + (value % 10U));
        value = (uint16_t)(value / 10U);
        ++length;
    } while ((value != 0U) && (length < 5U));

    while (length < minimum_digits)
    {
        text[length] = '0';
        ++length;
    }
    text[length] = '\0';

    for (index = 0U; index < (length / 2U); ++index)
    {
        digit = text[index];
        text[index] = text[length - index - 1U];
        text[length - index - 1U] = digit;
    }

    LCD_DrawString(x, y, text, scale, color);
}

void LCD_ShowOriginalContent(void)
{
    LCD_Fill(LCD_COLOR_BLACK);
    LCD_DrawString(42U, 8U, "HEART MONITOR", 2U, LCD_COLOR_CYAN);
    LCD_DrawString(18U, 52U, "MODE", 2U, LCD_COLOR_WHITE);
    LCD_DrawString(18U, 136U, "BRIGHTNESS", 2U, LCD_COLOR_WHITE);
}
