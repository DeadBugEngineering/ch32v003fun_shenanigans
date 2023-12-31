/* This code builds on code from the ch32v003fun project. 
Firmware for HiVoPuCounter, a FRAM based counter module meant to be used to retrofit xenon-flashing devices with a 
means to persistently log long term usage data of a specific xenon-bulb to be able to gauge its remaining service life. 
The detection of discharging events are done by continuously sampling the high voltage across the discharge capacitor 
and checking for sudden drops of its voltage. Data binning is utilized (with the charging voltage as parameter) for 
storing the wear data of a specific xenon-bulb. The data is stored in a FRAM to avoid data loss as this module will be 
subjected to EMI due to being housed inside the xenon-flasher chassis. The aim is to evaluate if it is possible to omid 
costly shielding for this module and whether it is possible to write the firmware robust enough that the missed 
discharge events (due to the uC resetting itself from EMI) can be minimized.

This module is meant to be used with a xenon-flasher device that works with a HV between 400V..3200V @ 1Hz pulse
repetition rate. Following pseudo-code gives a overview of the logger, which consists of a main loop that runs at 1kHz.
Since the flasher runs only at 1Hz, the logger can output statistical data via the OLED-display and UART right after 
each flash, without losing the ability to detect the next discharge event.

Init();
while (1)
{
    --------------------------------------------------------------------------------------------------------------------
    Wait until 1ms has passed from the last iteration.
    --------------------------------------------------------------------------------------------------------------------
    Calculate the average of all samples in the "sample buffer".
    --------------------------------------------------------------------------------------------------------------------
    Take another sample of the HV level.
    --------------------------------------------------------------------------------------------------------------------
    Push the newest sample into the least significant position (sample_buffer[0]) after pushing all samples one 
    position deeper into the sample_buffer[]. The oldest sample gets pushed out back into the void.
    --------------------------------------------------------------------------------------------------------------------
    Check if a discharge event has occured. This is done by searching for dips in voltage compared to the average value.
    Whenever a discharge event is detected, a flag is set to disable the discharge detection to avoid douple detections
    of one discharge event. If this flag is set, the discharge event detector does nothing for that iteration.
    --------------------------------------------------------------------------------------------------------------------
    Check if the discharge event flag can be reset again after a detected discharge event. This is done by checking
    that all samples in buffer_samples[] are close to the average value -> Voltage across the discharge capacitor has
    stabilized after a discharge / charge event.
    --------------------------------------------------------------------------------------------------------------------
}

*/

#define SSD1306_128X64

#include "ch32v003fun.h"
#include "ch32v003_GPIO_branchless.h"
#include <stdio.h>
#include "ssd1306_i2c.h"
#include "ssd1306.h"
#include <stdint.h>

#ifndef APB_CLOCK
	#define APB_CLOCK FUNCONF_SYSTEM_CORE_CLOCK
#endif

/* some bit definitions for systick regs */
#define SYSTICK_SR_CNTIF (1<<0)
#define SYSTICK_CTLR_STE (1<<0)
#define SYSTICK_CTLR_STIE (1<<1)
#define SYSTICK_CTLR_STCLK (1<<2)
#define SYSTICK_CTLR_STRE (1<<3)
#define SYSTICK_CTLR_SWIE (1<<31)

#define CH32V003_SPI_IMPLEMENTATION
#define CH32V003_SPI_SPEED_HZ 12000000
#define CH32V003_SPI_DIRECTION_2LINE_TXRX
#define CH32V003_SPI_CLK_MODE_POL0_PHA0	
#define CH32V003_SPI_NSS_HARDWARE_PC0

// commands of the FRAM IC: FM25040B
#define OP_WREN     0b00000110
#define OP_WRDI     0b00000100
#define OP_RDSR     0b00000101
#define OP_WRSR     0b00000001
#define OP_READ_H   0b00001011
#define OP_READ_L   0b00000011
#define OP_WRITE_H  0b00001010
#define OP_WRITE_L  0b00000010

#define BUFFER_DEPTH 5 // amout of samples that get buffered in buffer_samples[]
#define DELTAV_TH 10 // 64 == 200V @ max voltage 3200V
// the amount of ADC increments that are tolerable around the avg value to unblock the discharge detection.
#define UNBLOCK_DELTA 2 
// After a discharge event is detected, the detection is suspended for 100ms to avoid double counting of discharges.
#define DETECTION_BLOCK_COUNTER_MAX 100

#include "ch32v003_SPI.h"

// const, generated by generator.py
const uint16_t bins_V[] = {420,448,476,504,532,560,588,616,644,672,700,728,756,785,813,841, 869,897,925,953,981,1009,
1037,1065,1093,1122,1150,1178,1206,1234,1262,1290,1318,1346,1374,1402,1430,1458,1487,1515,1543,1571,1599,1627,1655,1683,
1711,1739,1767,1795,1824,1852,1880,1908,1936,1964,1992,2020,2048,2076,2104,2132,2161,2189,2217,2245,2273,2301,2329,2357,
2385,2413,2441,2469,2497,2526,2554,2582,2610,2638,2666,2694,2722,2750,2778,2806,2834,2863,2891,2919,2947,2975,3003,3031,
3059,3087,3115,3143,3171,3200};

const uint16_t bins_adc[] = {134,143,152,161,170,179,188,197,206,215,224,233,241,250,259,268,277,286,295,304,313,322,
331,340,349,358,367,376,385,394,403,412,421,430,439,448,457,466,475,484,493,502,511,520,529,538,547,556,565,574,583,592,
601,610,619,628,636,645,654,663,672,681,690,699,708,717,726,735,744,753,762,771,780,789,798,807,816,825,834,843,852,861,
870,879,888,897,906,915,924,933,942,951,960,969,978,987,996,1005,1014,1023};

const uint16_t addr[] = {0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,
120,124,128,132,136,140,144,148,152,156,160,164,168,172,176,180,184,188,192,196,200,204,208,212,216,220,224,228,232,236,
240,244,248,252,256,260,264,268,272,276,280,284,288,292,296,300,304,308,312,316,320,324,328,332,336,340,344,348,352,356,
360,364,368,372,376,380,384,388,392,396};
// const end, generated by generator.py

// const
const uint16_t adc_value_tolerance = 5;

// global variables
uint16_t buffer_samples[BUFFER_DEPTH]; // used to calculate the average of ADC samples
uint32_t avg; // the average of the samples in the ADC sample buffer
// discharge detection is blocked after a discharge event until voltage stabilizes to avoid double-counting discharges
uint16_t discharge_detection_blocked; 
uint8_t text_string[20]; // buffer for OLED output
uint8_t string_buffer[20]; // is used by the int2str function
uint8_t current_row = 0; // this variable is used to control the output on the OLED display
uint8_t vstring[] = "V:";
// used to disable the detection of discharge events for n iterations after a discharge event was detected
uint32_t detection_block_counter; 
int32_t adc_value_delta = 0; // the amounts of ADC increments the samples differ from the avg value
volatile uint32_t systick_cnt;

// function protoypes
void write_4bytes(uint16_t addr, uint32_t data);    // write 4 bytes into FRAM starting at addr
uint32_t read_4bytes(uint16_t addr); // read 4 bytes into FRAM starting at addr
uint32_t JP6_PD3_asserted(void);
uint32_t JP7_PD2_asserted(void);
void disable_writeprotection(void);
void detect_discharge(void);
int16_t get_index_bin(uint16_t adc_value);
void dump_FRAM(uint16_t bins); // dumps the counts per bin via uart
void clear_FRAM(void);
void unblock_detect_discharge(void);
void debug_pulse_PC2(void);
void systick_init(void);
void output_log(void); // outputs the logged data via UART
void int2str(int32_t num, uint8_t* str, uint8_t base);
void concatenateStrings(uint8_t* dest, uint8_t* src);

uint32_t JP6_PD3_asserted(void){
    return (GPIOD->INDR & 0x00000008);
}

uint32_t JP7_PD2_asserted(void){
    return (GPIOD->INDR & 0x00000004);
}

void write_4bytes(uint16_t addr, uint32_t data){
    SPI_begin_8();
    SPI_transfer_8(OP_WREN); // disable write protection of the FRAM
    SPI_end();
    if (addr < 0x1FC){ 
	    SPI_begin_8();
        if (addr > 255){
            SPI_transfer_8(OP_WRITE_H);
        }
        else {
            SPI_transfer_8(OP_WRITE_L);
        }
        SPI_transfer_8(addr);
        SPI_transfer_8(data>>24);
        SPI_transfer_8(data>>16);
        SPI_transfer_8(data>>8);
        SPI_transfer_8(data);
	    SPI_end();
    }
}

uint32_t read_4bytes(uint16_t addr){
    uint32_t data = 0;
    SPI_begin_8();
    if (addr > 255){
        SPI_transfer_8(OP_READ_H);
    }
    else {
        SPI_transfer_8(OP_READ_L);
    }
    SPI_transfer_8(addr);
    data = SPI_transfer_8(0x00);
    data <<= 8;
    data += SPI_transfer_8(0x00);
    data <<= 8;
    data += SPI_transfer_8(0x00);
    data <<= 8;
    data += SPI_transfer_8(0x00);
    SPI_end();
    return data;
}

uint8_t get_status_byte(void){
    uint8_t data;
    SPI_begin_8();
    SPI_transfer_8(OP_RDSR);
    data = SPI_transfer_8(0x00);
    SPI_end();
    return data;
}


void detect_discharge(void){
    if (!discharge_detection_blocked){
        if (avg-DELTAV_TH > buffer_samples[0]){ // voltage dipped relative to average value
            discharge_detection_blocked = 1;
            if (get_index_bin(avg)!= -1){
                // increment the bin for the current avg value after detecting dip in HV
                write_4bytes(addr[get_index_bin(avg)], read_4bytes(addr[get_index_bin(avg)]) + 1); 
                output_log(); // uart output
                if(current_row <= 6){ // determine the OLED row for the next line of log output on 8 line OLED display.
                    current_row++;
                }
                else {
                    current_row = 0;
                    ssd1306_setbuf(0);
                }
                int2str(bins_V[get_index_bin(avg)], text_string, 10);
                concatenateStrings(text_string, vstring);
                int2str(read_4bytes(addr[get_index_bin(avg)]), string_buffer, 10);
                concatenateStrings(text_string, string_buffer);
                ssd1306_drawstr(0,8*current_row, text_string, 1);
                ssd1306_refresh();
            }
        }
    }
}


int16_t get_index_bin(uint16_t adc_value){
    int16_t i;
    for (i=(sizeof(bins_adc)/sizeof(bins_adc[0]))-1; i>0; i--){
        if(adc_value > bins_adc[i]){
            return i;
        }
    }
    return -1;
}


/*
 * initialize adc for polling
 */
void adc_init( void )
{
	// ADCCLK = 24 MHz => RCC_ADCPRE = 0: divide by 2
	RCC->CFGR0 &= ~(0x1F<<11);
	// Enable GPIOD and ADC
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1;
	// PD4 is analog input chl 7
	GPIOD->CFGLR &= ~(0xf<<(4*4));	// CNF = 00: Analog, MODE = 00: Input
	// Reset the ADC to init all regs
	RCC->APB2PRSTR |= RCC_APB2Periph_ADC1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_ADC1;
	// Set up single conversion on chl 7
	ADC1->RSQR1 = 0;
	ADC1->RSQR2 = 0;
	ADC1->RSQR3 = 7;	// 0-9 for 8 ext inputs and two internals
	// set sampling time for chl 7
	ADC1->SAMPTR2 &= ~(ADC_SMP0<<(3*7));
	ADC1->SAMPTR2 |= 7<<(3*7);	// 0:7 => 3/9/15/30/43/57/73/241 cycles
	// turn on ADC and set rule group to sw trig
	ADC1->CTLR2 |= ADC_ADON | ADC_EXTSEL;
	// Reset calibration
	ADC1->CTLR2 |= ADC_RSTCAL;
	while(ADC1->CTLR2 & ADC_RSTCAL);
	// Calibrate
	ADC1->CTLR2 |= ADC_CAL;
	while(ADC1->CTLR2 & ADC_CAL);
	// should be ready for SW conversion now
}

/*
 * start conversion, wait and return result
 */
uint16_t adc_get( void )
{
	// start sw conversion (auto clears)
	ADC1->CTLR2 |= ADC_SWSTART;
	// wait for conversion complete
	while(!(ADC1->STATR & ADC_EOC));
	// get result
	return ADC1->RDATAR;
}

uint32_t calc_avg(void){
    uint32_t i;
    uint32_t sum = 0;
    uint32_t avg = 0;
    for (i=0; i<BUFFER_DEPTH; i++){
        sum += buffer_samples[i];
    }
    avg = sum / BUFFER_DEPTH;
    return avg;
}

void dump_FRAM(uint16_t bins){
    uint32_t i = 0;
    for (i=0; i<=bins; i+=4){
        printf( "addr: %lu \t :%lu \n", i, read_4bytes(i));   
    }
}

void clear_FRAM(void){
    for (uint16_t i=0; i<=1023; i+=4){
        write_4bytes(i, 0);
    }
}

// unblock discharge detection if current sample is withing avg+-UNBLOCK_DELTA for consecutive samples
void unblock_detect_discharge(void){ 
    if ((buffer_samples[0] < (avg + UNBLOCK_DELTA))&&(buffer_samples[0] > (avg - UNBLOCK_DELTA))){
        detection_block_counter++;
        if (detection_block_counter > DETECTION_BLOCK_COUNTER_MAX){
            detection_block_counter = 0;
            discharge_detection_blocked = 0;
        }
    }
}

void debug_pulse_PC2(void){
    GPIO_digitalWrite_hi(GPIOv_from_PORT_PIN(GPIO_port_C, 2));
    GPIO_digitalWrite_lo(GPIOv_from_PORT_PIN(GPIO_port_C, 2));
}

/*
 * Start up the SysTick IRQ
 */
void systick_init(void)
{
	/* disable default SysTick behavior */
	SysTick->CTLR = 0;
	/* enable the SysTick IRQ */
	NVIC_EnableIRQ(SysTicK_IRQn);
	/* Set the tick interval to 1ms for normal op */
	SysTick->CMP = (FUNCONF_SYSTEM_CORE_CLOCK/1000)-1;
	/* Start at zero */
	SysTick->CNT = 0;
	systick_cnt = 0;
	/* Enable SysTick counter, IRQ, HCLK/1 */
	SysTick->CTLR = SYSTICK_CTLR_STE | SYSTICK_CTLR_STIE |
					SYSTICK_CTLR_STCLK;
}

/*
 * SysTick ISR just counts ticks
 * note - the __attribute__((interrupt)) syntax is crucial!
 */
void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void)
{
	// move the compare further ahead in time.
	// as a warning, if more than this length of time
	// passes before triggering, you may miss your
	// interrupt.
	SysTick->CMP += (FUNCONF_SYSTEM_CORE_CLOCK/1000);
	/* clear IRQ */
	SysTick->SR = 0;
	/* update counter */
	systick_cnt++;
}

void output_log(void){
    uint16_t i;
    uint32_t buffer;

    printf( "-------------------------------\n");
    printf( "avg:%uV\n", bins_V[get_index_bin(avg)]);
    for (i=0; i<=(sizeof(addr)/sizeof(addr[0]))-1; i+=1){
        buffer = read_4bytes(addr[i]);
        if(buffer>0){
            printf( "%u[V]:%lu\n", bins_V[i], buffer);
        }
    }
}

void int2str(int32_t num, uint8_t* str, uint8_t base) {
    int32_t tempNum = num;
    int8_t digit;
    uint8_t i = 0;
    uint8_t isNegative = 0;

    // Handle negative numbers
    if (num < 0) {
        isNegative = 1;
        tempNum = -num;
    }

    // Process digits in reverse order
    do {
        digit = tempNum % base;
        str[i++] = (digit < 10) ? (digit + '0') : (digit - 10 + 'A');
        tempNum /= base;
    } while (tempNum > 0);

    // Add negative sign if needed
    if (isNegative) {
        str[i++] = '-';
    }

    // Add null terminator
    str[i] = '\0';

    // Reverse the string
    uint8_t start = 0;
    uint8_t end = i - 1;
    while (start < end) {
        // Swap characters
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;

        // Move towards the center
        start++;
        end--;
    }
}


void concatenateStrings(uint8_t* dest, uint8_t* src) {
    // Find the end of the destination string
    while (*dest != '\0')
    {
        dest++;
    }

    // Copy characters from src to dest
    while (*src != '\0')
    {
        *dest = *src;
        dest++;
        src++;
    }

    // Add null terminator
    *dest = '\0';
}

uint32_t stat_data[320]; // 0==number flash between 0V and 10V, 1==11V..20, etc
//const uint32_t threshhold_values[]={}

int main()
{
    int32_t i;

    discharge_detection_blocked = 0;
    detection_block_counter = 0;
	SystemInit();
    systick_init();
    adc_init();
	// Enable GPIOD.
    GPIO_port_enable(GPIO_port_D)
	// Enable GPIOC.
    GPIO_port_enable(GPIO_port_C)
	// Enable SPI1 clock
	RCC->APB2PCENR |= RCC_APB2Periph_SPI1;
	// Enable aux func clock
	RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
    GPIO_pinMode(GPIOv_from_PORT_PIN(GPIO_port_D, 0), GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);

    // Configure GPIOs
    GPIO_pinMode(GPIOv_from_PORT_PIN(GPIO_port_D, 2), GPIO_pinMode_I_pullUp, GPIO_Speed_10MHz);
    GPIO_pinMode(GPIOv_from_PORT_PIN(GPIO_port_D, 3), GPIO_pinMode_I_pullUp, GPIO_Speed_10MHz);
    GPIO_pinMode(GPIOv_from_PORT_PIN(GPIO_port_C, 1), GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);
    GPIO_pinMode(GPIOv_from_PORT_PIN(GPIO_port_C, 2), GPIO_pinMode_O_pushPull, GPIO_Speed_10MHz);

	SPI_init();
    ssd1306_i2c_init(); 
    ssd1306_init();
    //clear_FRAM();
	while(1)
	{
        avg = calc_avg(); // calculate the average in the sample buffer
        // push ADC-samples down the cue
        for (i=BUFFER_DEPTH-1; i>=0; i--)
        {
            buffer_samples[i+1] = buffer_samples[i];
        }
        // get current sample and push into buffer
        buffer_samples[0] = adc_get();
        detect_discharge();
        unblock_detect_discharge(); // 
        while (systick_cnt == 0); // wait for IRQ to inc systick_cnt
        systick_cnt = 0;
	}
}
