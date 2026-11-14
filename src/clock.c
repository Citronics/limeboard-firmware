// System clock: 84 MHz core, 48 MHz USB, from the external crystal.
//
// The crystal's frequency is measured at boot instead of assumed: TIM11 can
// capture the crystal clock divided by RTCPRE (HSE_RTC) while running from
// the 16 MHz internal oscillator, as ST's own bootloader does.
#include "board.h"

#define HSI_HZ 16000000u  // internal oscillator: the reference for the measurement

static const uint8_t known_mhz[] = {8, 12, 16, 20, 24, 25, 26};

// Crystal frequency in MHz, or 0 without a working crystal
static uint32_t measure_hse_mhz(void)
{
	RCC->CR |= RCC_CR_HSEON;
	for (uint32_t i = 0; !(RCC->CR & RCC_CR_HSERDY); i++)
		if (i > 1000000)
			return 0;

	// HSE_RTC = HSE / 16 into TIM11 channel 1, every 8th edge captured
	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_RTCPRE) | (16u << RCC_CFGR_RTCPRE_Pos);
	RCC->APB2ENR |= RCC_APB2ENR_TIM11EN;
	(void)RCC->APB2ENR;
	TIM11->OR = 3;  // TI1 <- HSE_RTC
	TIM11->PSC = 0;
	TIM11->ARR = 0xFFFF;
	TIM11->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_IC1PSC;
	TIM11->CCER = TIM_CCER_CC1E;
	TIM11->CR1 = TIM_CR1_CEN;

	uint32_t sum = 0;
	uint16_t last = 0;
	for (int n = -1; n < 32; n++) {
		TIM11->SR = 0;
		for (uint32_t i = 0; !(TIM11->SR & TIM_SR_CC1IF); i++)
			if (i > 100000)
				return 0;
		uint16_t now = TIM11->CCR1;
		if (n >= 0)
			sum += (uint16_t)(now - last);
		last = now;
	}
	TIM11->CR1 = 0;
	RCC->APB2ENR &= ~RCC_APB2ENR_TIM11EN;

	// 32 captures x 8 edges of HSE/16, counted in 16 MHz ticks
	uint32_t hz = (uint32_t)((uint64_t)HSI_HZ * 16 * 8 * 32 / sum);
	for (unsigned i = 0; i < sizeof(known_mhz); i++) {
		uint32_t f = known_mhz[i] * 1000000u;
		if (hz > f - f / 33 && hz < f + f / 33)  // within 3 %
			return known_mhz[i];
	}
	return 0;
}

uint32_t clock_init(void)
{
	uint32_t hse_mhz = measure_hse_mhz();

	// PLL input 1 MHz: VCO 336 MHz, /4 = 84 MHz core, /7 = 48 MHz USB
	uint32_t src = hse_mhz ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI;
	uint32_t m = hse_mhz ? hse_mhz : 16;
	if (!hse_mhz)
		RCC->CR &= ~RCC_CR_HSEON;
	RCC->PLLCFGR = src | (m << RCC_PLLCFGR_PLLM_Pos) | (336u << RCC_PLLCFGR_PLLN_Pos) |
		       (1u << RCC_PLLCFGR_PLLP_Pos) | (7u << RCC_PLLCFGR_PLLQ_Pos);
	RCC->CR |= RCC_CR_PLLON;
	while (!(RCC->CR & RCC_CR_PLLRDY)) {
	}

	FLASH->ACR = FLASH_ACR_LATENCY_2WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
	// AHB 84 MHz, APB1 42 MHz (its maximum), APB2 84 MHz
	RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW)) |
		    RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_SW_PLL;
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
	}

	SystemCoreClock = 84000000;
	SysTick_Config(SystemCoreClock / 1000);
	return hse_mhz;
}
