// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#include "nora_ccp_input_capture_dspic33ak_reg.h"

int main(void)
{
    volatile uint32_t reg = 0u;
    dspic33ak_ccp_reg_set_or_clear(&reg, DSPIC33AK_CCP_CON1_T32, true);
    return reg == DSPIC33AK_CCP_CON1_T32 ? 0 : 1;
}
