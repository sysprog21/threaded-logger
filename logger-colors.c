#include "colors.h"
#include "logger.h"

const logger_line_colors_t logger_colors_default = {
    .level =
        {
            [LOGGER_LEVEL_ALL] = "", /* Default. In case we miss one ... */

            [LOGGER_LEVEL_ALERT] = C_UNL C_LR,
            [LOGGER_LEVEL_CRITICAL] = C_LR,
            [LOGGER_LEVEL_ERROR] = C_DR,
            [LOGGER_LEVEL_WARNING] = C_DY,
            [LOGGER_LEVEL_NOTICE] = C_DW,
            [LOGGER_LEVEL_INFO] = C_DB,
            [LOGGER_LEVEL_DEBUG] = C_DM,
            [LOGGER_LEVEL_TRACE] = C_DC,
        },
    .reset = C_RST,
    .time = "",
    .date = C_LG,
    .date_lines = C_DG,
    .thread_name = C_DW,
};
