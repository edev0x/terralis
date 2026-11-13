#ifndef FIRMWARE_DISPLAY_H
#define FIRMWARE_DISPLAY_H

#include <Arduino.h>
#include <stdarg.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "config.h"
#include "constants.h"
#include "pins.h"
#include "utils.h"

// Paginas de la pantalla OLED de 128x64. Solo dibuja: los pulsadores y el
// mutex del bus I2C los maneja la tarea tUI (main.cpp).
namespace Display
{
    enum Page
    {
        PAGE_WEATHER,
        PAGE_SOIL,
        PAGE_IRRIGATION,
        PAGE_RICE,
        PAGE_RISK,
        PAGE_STAGE_COFFEE, // las tres ultimas paginas cambian la etapa de un cultivo
        PAGE_STAGE_BEAN,
        PAGE_STAGE_RICE,
        N_PAGES
    };

    inline bool isStagePage(int page) { return page >= PAGE_STAGE_COFFEE; }
    inline int cropOfPage(int page) { return page - PAGE_STAGE_COFFEE; }

    // Indicadores de la esquina superior derecha
    struct Status
    {
        bool sdOk, netOk, alarmActive, snoozed;
    };

    // Escribe lineas de texto una debajo de otra
    struct Screen
    {
        Adafruit_SSD1306 &oled;
        int y;

        void line(const char *txt)
        {
            oled.setCursor(0, y);
            oled.print(txt);
            y += 9;
        }

        __attribute__((format(printf, 2, 3))) void linef(const char *fmt, ...)
        {
            char l[40];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(l, sizeof(l), fmt, ap);
            va_end(ap);
            line(l);
        }
    };

    inline const char *onOff(const Utils::Snapshot &s, uint8_t bit, const char *off)
    {
        return (s.outputs & (1u << bit)) ? "ON" : off;
    }

    inline void pageWeather(Screen &scr, const Utils::Snapshot &s)
    {
        char stamp[24];
        Time::date_text(s.epoch, stamp, sizeof(stamp));
        scr.line(stamp);
        if (isnan(s.t))
            scr.line("T --  HR --");
        else
            scr.linef("T %.1fC  HR %.0f%%", s.t, s.hr);
        scr.linef("P %.0f hPa  Luz %.0f%%", s.p, s.rad);
        scr.linef("Epoca: %s", Constants::season_name(s.season));
        scr.linef("Hoja: %s", s.leaf ? "mojada" : "seca");
    }

    inline void pageSoil(Screen &scr, const Utils::Snapshot &s)
    {
        scr.linef("Suelo cafe   %.0f%%", s.coffee_soil);
        scr.linef("Suelo frijol %.0f%%", s.bean_soil);
        scr.linef("Lluvia dia %.1f mm", s.rain_day);
        scr.linef("Lluvia 6 h %.1f mm", s.rain_6h);
        scr.linef("ET0 %.2f  D %.0f/%.0f", s.et0, s.d_coffee, s.d_bean);
    }

    inline void pageIrrigation(Screen &scr, const Utils::Snapshot &s)
    {
        char blocks[24];
        Constants::blockage_text(s.blockages, blocks, sizeof(blocks));
        scr.linef("Cafe:   %s", Constants::irrigation_state(s.state_coffee));
        scr.linef("Frijol: %s", Constants::irrigation_state(s.state_bean));
        scr.linef("Bomba: %s", onOff(s, Pins::Out::PUMP, "OFF"));
        scr.linef("Bloq: %s", blocks);
        scr.linef("Reservorio %.0f%%", s.reservoir);
    }

    inline void pageRice(Screen &scr, const Utils::Snapshot &s)
    {
        scr.linef("Lamina %.1f / %.1f cm", s.layer, s.layer_target);
        scr.linef("Estado: %s", Constants::layer_name(s.state_rice));
        scr.linef("Entrada %s Drenaje %s", onOff(s, Pins::Out::V_RICE_IN, "--"),
                  onOff(s, Pins::Out::V_RICE_OUT, "--"));
        scr.line(Config::stage_name(Constants::CROP_RICE, s.stage[Constants::CROP_RICE]));
        scr.linef("Reservorio %.0f%%", s.reservoir);
    }

    inline void pageRisk(Screen &scr, const Utils::Snapshot &s)
    {
        scr.linef("Cafe (roya)  %s %uh", Constants::risk_detail(s.risk[0]), (unsigned)s.h48[0]);
        scr.linef("Frijol       %s %uh", Constants::risk_detail(s.risk[1]), (unsigned)s.h48[1]);
        scr.linef("Arroz (anub) %s %uh", Constants::risk_detail(s.risk[2]), (unsigned)s.h48[2]);
        scr.line("Horas favorables 48 h");
        if (s.safe)
            scr.line("MODO SEGURO!");
    }

    inline void pageStage(Screen &scr, const Utils::Snapshot &s, int crop)
    {
        scr.line("Etapa actual:");
        scr.line(Config::stage_name(crop, s.stage[crop]));
        scr.line("");
        scr.line("OK corto: siguiente");
        scr.line("OK largo: silenciar");
    }

    // Dibuja la pagina indicada. hasData = false mientras no llega el primer Snapshot.
    inline void draw(Adafruit_SSD1306 &oled, const Utils::Snapshot &s, bool hasData, int page,
                     const Status &st)
    {
        static const char *const TITLES[N_PAGES] = {"CLIMA", "SUELO", "RIEGO", "ARROZ", "RIESGO",
                                                    "ETAPA CAFE", "ETAPA FRIJOL", "ETAPA ARROZ"};
        char title[40];
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        snprintf(title, sizeof(title), "%d/%d %s", page + 1, (int)N_PAGES, TITLES[page]);
        oled.print(title);

        // S = microSD, R = red, ! = alarma, x = alarma silenciada
        oled.setCursor(104, 0);
        oled.print(st.sdOk ? 'S' : '-');
        oled.print(st.netOk ? 'R' : '-');
        oled.print(st.alarmActive ? '!' : (st.snoozed ? 'x' : ' '));

        Screen scr = {oled, 12};
        if (!hasData)
        {
            scr.y = 16;
            scr.line("Esperando datos...");
        }
        else
        {
            switch (page)
            {
            case PAGE_WEATHER:
                pageWeather(scr, s);
                break;
            case PAGE_SOIL:
                pageSoil(scr, s);
                break;
            case PAGE_IRRIGATION:
                pageIrrigation(scr, s);
                break;
            case PAGE_RICE:
                pageRice(scr, s);
                break;
            case PAGE_RISK:
                pageRisk(scr, s);
                break;
            default:
                pageStage(scr, s, cropOfPage(page));
                break;
            }
        }
        oled.display();
    }

    // Pantalla de arranque
    inline void splash(Adafruit_SSD1306 &oled)
    {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.println("Estacion agromet.");
        oled.println("Cafe/Frijol/Arroz");
        oled.println("Iniciando...");
        oled.display();
    }
}

#endif