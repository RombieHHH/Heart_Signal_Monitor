#ifndef APPLICATION_LCD_H
#define APPLICATION_LCD_H

#include <stdint.h>

#define LCD_WIDTH  240U
#define LCD_HEIGHT 240U

#define LCD_COLOR_BLACK   0x0000U
#define LCD_COLOR_WHITE   0xFFFFU
#define LCD_COLOR_RED     0xF800U
#define LCD_COLOR_GREEN   0x07E0U
#define LCD_COLOR_BLUE    0x001FU
#define LCD_COLOR_CYAN    0x07FFU
#define LCD_COLOR_YELLOW  0xFFE0U
#define LCD_COLOR_MAGENTA 0xF81FU

void LCD_Init(void);
void LCD_SetBacklight(uint8_t enabled);
void LCD_Fill(uint16_t color);
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                  uint16_t color);
void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawString(uint16_t x, uint16_t y, const char *text, uint8_t scale,
                    uint16_t color);
void LCD_ShowOriginalContent(void);

#endif
