#ifndef FIRMWARE_LOGGER_HANDLER_H
#define FIRMWARE_LOGGER_HANDLER_H

#include "utils.h"
#include "constants.h"

class Logger
{
public:
    Logger() = delete;

    static inline const char *csv_header()
    {
        return "timestamp,t_c,hr_pct,p_hpa,rad_pct,lluvia_mm,suelo_cafe,suelo_frijol,lamina_cm,"
               "reservorio_pct,estado_cafe,estado_frijol,estado_arroz,riesgo_cafe,riesgo_frijol,"
               "riesgo_arroz";
    }

    static inline void format_csv(const Utils::Snapshot &s, char *buf, size_t n)
    {
        char fh[24], t[12], hr[12], p[12], rad[12], ll[12], sc[12], sf[12], la[12], re[12];
        Utils::date_text(s.epoch, fh, sizeof(fh));
        Utils::num_txt(t, sizeof(t), s.t, 1, "");
        Utils::num_txt(hr, sizeof(hr), s.hr, 0, "");
        Utils::num_txt(p, sizeof(p), s.p, 0, "");
        Utils::num_txt(rad, sizeof(rad), s.rad, 0, "");
        Utils::num_txt(ll, sizeof(ll), s.rain_day, 1, "");
        Utils::num_txt(sc, sizeof(sc), s.coffee_soil, 0, "");
        Utils::num_txt(sf, sizeof(sf), s.bean_soil, 0, "");
        Utils::num_txt(la, sizeof(la), s.layer, 1, "");
        Utils::num_txt(re, sizeof(re), s.reservoir, 0, "");
        snprintf(buf, n, "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s", fh, t, hr, p, rad, ll, sc,
                 sf, la, re, Constants::irrigation_state(s.est_coffee), Constants::irrigation_state(s.est_bean),
                 Constants::layer_name(s.est_rice), Constants::risk_detail(s.risk[0]), Constants::risk_detail(s.risk[1]),
                 Constants::risk_detail(s.risk[2]));
    }

    static inline void format_json(const Utils::Snapshot &s, char *buf, size_t n)
    {
        char fh[24], t[12], hr[12], p[12], rad[12], l6[12], ld[12], sc[12], sf[12], la[12], lo[12],
            re[12], et[12], dc[12], df[12];

        Utils::date_text(s.epoch, fh, sizeof(fh));
        Utils::num_txt(t, sizeof(t), s.t, 1, "null");
        Utils::num_txt(hr, sizeof(hr), s.hr, 0, "null");
        Utils::num_txt(p, sizeof(p), s.p, 1, "null");
        Utils::num_txt(rad, sizeof(rad), s.rad, 0, "null");
        Utils::num_txt(ld, sizeof(ld), s.rain_day, 1, "null");
        Utils::num_txt(l6, sizeof(l6), s.rain_6h, 1, "null");
        Utils::num_txt(sc, sizeof(sc), s.coffee_soil, 0, "null");
        Utils::num_txt(sf, sizeof(sf), s.bean_soil, 0, "null");
        Utils::num_txt(la, sizeof(la), s.layer, 1, "null");
        Utils::num_txt(lo, sizeof(lo), s.layer_obj, 1, "null");
        Utils::num_txt(re, sizeof(re), s.reservoir, 0, "null");
        Utils::num_txt(et, sizeof(et), s.et0, 2, "null");
        Utils::num_txt(dc, sizeof(dc), s.d_coffe, 1, "null");
        Utils::num_txt(df, sizeof(df), s.d_bean, 1, "null");
        snprintf(buf, n,
                 "{\"timestamp\":\"%s\",\"t\":%s,\"hr\":%s,\"p\":%s,\"rad\":%s,"
                 "\"lluvia_dia\":%s,\"lluvia_6h\":%s,\"suelo_cafe\":%s,\"suelo_frijol\":%s,"
                 "\"hoja\":%u,\"lamina\":%s,\"lamina_obj\":%s,\"reservorio\":%s,"
                 "\"estado\":{\"cafe\":\"%s\",\"frijol\":\"%s\",\"arroz\":\"%s\"},"
                 "\"riesgo\":{\"cafe\":\"%s\",\"frijol\":\"%s\",\"arroz\":\"%s\"},"
                 "\"h48\":[%u,%u,%u],\"etapa\":[%u,%u,%u],\"epoca\":\"%s\","
                 "\"bloqueos\":%u,\"fallas\":%u,\"seguro\":%u,\"salidas\":%u,"
                 "\"et0\":%s,\"d_cafe\":%s,\"d_frijol\":%s}",
                 fh, t, hr, p, rad, ld, l6, sc, sf, (unsigned)s.layer, la, lo, re,
                 Constants::irrigation_state(s.est_coffee), Constants::irrigation_state(s.est_bean), Constants::layer_name(s.est_rice),
                 Constants::risk_detail(s.risk[0]), Constants::risk_detail(s.risk[1]), Constants::risk_detail(s.risk[2]),
                 (unsigned)s.h48[0], (unsigned)s.h48[1], (unsigned)s.h48[2], (unsigned)s.stage[0],
                 (unsigned)s.stage[1], (unsigned)s.stage[2], Constants::time_of_year(s.season),
                 (unsigned)s.blockages, (unsigned)s.failures, (unsigned)s.safe, (unsigned)s.exits, et,
                 dc, df);
    }
};

#endif