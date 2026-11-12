#ifndef FIRMWARE_DATETIME_H
#define FIRMWARE_DATETIME_H

#include <stdint.h>

class Time
{
public:
    Time() = delete;

    struct Date
    {
        int year, month, day, hh, mi, ss;
    };

    static inline Date from_epoch(uint32_t epoch)
    {
        uint32_t days = epoch / 86400u, sec = epoch % 86400u;
        int64_t z = (int64_t)days + 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;

        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;

        int64_t y = (int64_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp < 10 ? mp + 3 : mp - 9;

        if (m <= 2)
            y += 1;

        Date date;
        date.year = (int)y;
        date.month = (int)m;
        date.day = (int)d;
        date.hh = (int)(sec / 3600);
        date.mi = (int)((sec % 3600) / 60);
        date.ss = (int)(sec % 60);

        return date;
    }

    static inline int day_of_year(int y, int m, int d)
    {
        static const int acum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        int n = acum[m - 1] + d;
        bool bis = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        
        if (bis && m > 2) n++;
        return n;
    }
};

#endif