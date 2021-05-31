/************************************************************************/
/* includes                                                             */
/************************************************************************/

#include <asf.h>
#include <string.h>
#include "arm_math.h"
#include "math.h"
#include "ili9341.h"
#include "lvgl.h"
#include "touch/touch.h"
#include "cafe_image.h"

/************************************************************************/
/* STATIC                                                               */
/************************************************************************/

/*A static or global variable to store the buffers*/
static lv_disp_buf_t disp_buf;

/*Static or global buffer(s). The second buffer is optional*/
static lv_color_t buf_1[LV_HOR_RES_MAX * LV_VER_RES_MAX];

/************************************************************************/
/* RTOS                                                                 */
/************************************************************************/

#define TASK_LCD_STACK_SIZE                (1024*6/sizeof(portSTACK_TYPE))
#define TASK_LCD_STACK_PRIORITY            (tskIDLE_PRIORITY)

#define TASK_APS2_STACK_SIZE                (1024*6/sizeof(portSTACK_TYPE))
#define TASK_APS2_STACK_PRIORITY            (tskIDLE_PRIORITY)

#define LED_PIO      PIOC
#define LED_PIO_ID   ID_PIOC
#define LED_IDX      8
#define LED_IDX_MASK (1 << LED_IDX)

extern void vApplicationStackOverflowHook(xTaskHandle *pxTask,  signed char *pcTaskName);
extern void vApplicationIdleHook(void);
extern void vApplicationTickHook(void);
extern void vApplicationMallocFailedHook(void);
extern void xPortSysTickHandler(void);

extern void vApplicationStackOverflowHook(xTaskHandle *pxTask, signed char *pcTaskName) {
	printf("stack overflow %x %s\r\n", pxTask, (portCHAR *)pcTaskName);
	for (;;) {	}
}

extern void vApplicationIdleHook(void) { }

extern void vApplicationTickHook(void) { }

extern void vApplicationMallocFailedHook(void) {
	configASSERT( ( volatile void * ) NULL );
}

/************************************************************************/
/* STRUCTS                                                              */
/************************************************************************/
typedef struct  {
	uint32_t year;
	uint32_t month;
	uint32_t day;
	uint32_t week;
	uint32_t hour;
	uint32_t minute;
	uint32_t second;
} calendar;

calendar currentTime;


/************************************************************************/
/* prototypes */
/************************************************************************/

static void send_package(char data[], char n);
void lv_debug(void);
void lv_page_1_inicial(void);
void lv_page_2_configurando(void);
void lv_page_3_pagamento(void);
void lv_page_4_preparando(void);
void lv_page_canceled(void);
void lv_debug(void);

void RTC_init(Rtc *rtc, uint32_t id_rtc, calendar t, uint32_t irq_type);
void RTC_Handler(void);

/************************************************************************/
/* globais                                                                */
/************************************************************************/

QueueHandle_t xQueueRx;
QueueHandle_t xQueueProduto;
SemaphoreHandle_t xSemaphoreOk;
SemaphoreHandle_t xSemaphorePago;
SemaphoreHandle_t xSemaphoreSuccess;
SemaphoreHandle_t xSemaphoreCancel;
SemaphoreHandle_t xSemaphoreFail;
SemaphoreHandle_t xSemaphoreVerifica;

QueueHandle_t xQueueFeedback;

// LVGL globals
lv_obj_t * labelDebug;
lv_obj_t * bar_regulagem;
lv_obj_t * label_aguardando;
lv_obj_t * bar_preparando;
lv_obj_t * label_valor_regulagem;
lv_obj_t * label_valor;
static lv_obj_t * label_valor_timer;

int price = 0;

// PARA O TIMER 
volatile int flag_rtc = 1;
volatile int contador = 10;
volatile int troca_tela;

// PARA ANIMACAO 
volatile int preparando = 1;
/************************************************************************/
/* RTC                                                                */
/************************************************************************/
void RTC_init(Rtc *rtc, uint32_t id_rtc, calendar t, uint32_t irq_type){
	/* Configura o PMC */
	pmc_enable_periph_clk(ID_RTC);

	/* Default RTC configuration, 24-hour mode */
	rtc_set_hour_mode(rtc, 0);

	/* Configura data e hora manualmente */
	rtc_set_date(rtc, t.year, t.month, t.day, t.week);
	rtc_set_time(rtc, t.hour, t.minute, t.second);

	/* Configure RTC interrupts */
	NVIC_DisableIRQ(id_rtc);
	NVIC_ClearPendingIRQ(id_rtc);
	NVIC_SetPriority(id_rtc, 4);
	NVIC_EnableIRQ(id_rtc);

	/* Ativa interrupcao via alarme */
	rtc_enable_interrupt(rtc,  irq_type);
}

void RTC_Handler(void) {
	uint32_t ul_status = rtc_get_status(RTC);
	/* seccond tick */
	if ((ul_status & RTC_SR_SEC) == RTC_SR_SEC) {
		// 		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		// 		xSemaphoreGiveFromISR(xSemaphore, &xHigherPriorityTaskWoken);
	}
	/* Time or date alarm */
	if ((ul_status & RTC_SR_ALARM) == RTC_SR_ALARM) {
		flag_rtc = 1;
	}
	rtc_clear_status(RTC, RTC_SCCR_SECCLR);
	rtc_clear_status(RTC, RTC_SCCR_ALRCLR);
	rtc_clear_status(RTC, RTC_SCCR_ACKCLR);
	rtc_clear_status(RTC, RTC_SCCR_TIMCLR);
	rtc_clear_status(RTC, RTC_SCCR_CALCLR);
	rtc_clear_status(RTC, RTC_SCCR_TDERRCLR);
}

/************************************************************************/
/* handlers                                                             */
/************************************************************************/
void USART1_Handler(void) {
	uint32_t ret = usart_get_status(CONSOLE_UART);

	BaseType_t xHigherPriorityTaskWoken = pdTRUE;
	char c;

	if(ret & US_IER_RXRDY){
		usart_serial_getchar(CONSOLE_UART, &c);
		xQueueSendFromISR(xQueueRx, (void *) &c, &xHigherPriorityTaskWoken);

		// -  Transmissoa finalizada
		} else if(ret & US_IER_TXRDY){
	}
}

/************************************************************************/
/* TASKS                                                                */
/************************************************************************/

static void task_lcd(void *pvParameters) {
	//lv_debug();
	for (;;)  {
		lv_tick_inc(50);
		lv_task_handler();
		vTaskDelay(50);
	}
}

static void task_receive(void *pvParameters) {
	char p_data[32];
	char p_cnt = 0;
	char p_do = 0;
	int feedback_response = 10; //qualquer numero que nao seja 0, 1, 2 e 255
	
	for (;;)  {
		char c;
		if (xQueueReceive( xQueueRx, &c, 0 )) {
			p_data[p_cnt++] = c;
			if (p_data[p_cnt - 1] == 'X'){
				p_do = 1;
			}
		}
		
		if (p_do == 1) {
			if(p_data[2] == 255){
				feedback_response = 255;
				xQueueSend(xQueueFeedback, &feedback_response, 1000);
				//xSemaphoreGive(xSemaphoreSuccess);
			}
			
			if(p_data[2] == 0){
				feedback_response = 0;
				xQueueSend(xQueueFeedback, &feedback_response, 1000);
				//xSemaphoreGive(xSemaphoreFail);
			}
			if(p_data[2] == 2){
				feedback_response = 2;
				xQueueSend(xQueueFeedback, &feedback_response, 1000);
				//xSemaphoreGive(xSemaphoreCancel);
			}
			//lv_label_set_text_fmt(labelDebug, "%02X %02X %02X %02X",p_data[0], p_data[1], p_data[2], p_data[3] );
			p_cnt = 0;
			p_do = 0;
		}

	}
}

static void task_main(void *pvParameters) {
	xSemaphoreOk = xSemaphoreCreateBinary();
	if (xSemaphoreOk == NULL) printf("Falha em criar o semaforo \n");
	
	xSemaphorePago = xSemaphoreCreateBinary();
	if (xSemaphorePago == NULL) printf("Falha em criar o semaforo \n");
	
	xSemaphoreSuccess = xSemaphoreCreateBinary();
	if (xSemaphoreSuccess == NULL) printf("Falha em criar o semaforo \n");

	xSemaphoreCancel = xSemaphoreCreateBinary();
	if (xSemaphoreCancel == NULL) printf("Falha em criar o semaforo \n");
	
	xSemaphoreFail = xSemaphoreCreateBinary();
	if (xSemaphoreFail == NULL) printf("Falha em criar o semaforo \n");
	
	xSemaphoreVerifica = xSemaphoreCreateBinary();
	if (xSemaphoreVerifica == NULL) printf("Falha em criar o semaforo \n");
	
	xQueueProduto = xQueueCreate(32, sizeof(int));

	int produto_id = 0;
	int produto_acucar = 0;
	int feedback = 5; //feedback nao pode ser 0, 1, 2 e 255;
	
	char handshake[] = {'U', 0, 0, 'X'};
	char payment_confirmed[] = {'U', 2, 0, 'X'};
	char verifica_conexao[] = {'U', 2, 0, 'X'};
	
	enum states {EXIBE_TELA1, INICIAL, EXIBE_TELA2, EXIBE_DEBUG, ANALISANDO, CONFIGURANDO, EXIBE_TELA3, PAGAMENTO, EXIBE_TELA4, PREPARANDO, EXIBE_TELA5, CANCELADO} state;
	for (;;)  {
		switch (state){

			case EXIBE_TELA1:
			lv_page_1_inicial();
			pio_set(LED_PIO, LED_IDX_MASK);
			preparando = 1;
			send_package(handshake, 4);
			if(xQueueReceive(xQueueFeedback, &feedback, 1000)){
				if(feedback == 255){
					pio_clear(LED_PIO, LED_IDX_MASK);
					state = INICIAL;
				}
			}
			break;
			
			case INICIAL:
			if( xQueueReceive(xQueueProduto, &produto_id, 1000 ) ){
				//state = EXIBE_TELA2;
				state = EXIBE_DEBUG;
				price = 12;
			}
			break;
			
			case EXIBE_DEBUG:
			lv_debug();
			state = ANALISANDO;
			break;
			
			// verifica conexao
			case ANALISANDO:
			send_package(verifica_conexao, 4);
			if(xQueueReceive(xQueueFeedback, &feedback, 1000)){
				if(feedback == 255){
					xSemaphoreGive(xSemaphoreVerifica);
				}
			}
			if( xSemaphoreTake(xSemaphoreVerifica, 1000) ){
				state = EXIBE_TELA2;
			}

			break;
			
			case EXIBE_TELA2:
			lv_page_2_configurando();
			state = CONFIGURANDO;
			break;

			case CONFIGURANDO:
			if( xSemaphoreTake(xSemaphoreOk, 1000) ){
				produto_acucar = lv_bar_get_value(bar_regulagem);
				char payment[] = {'U', 1, price, 'X'}; 
				send_package(payment, 4);
				if(xQueueReceive(xQueueFeedback, &feedback, 1000)){
					if(feedback == 255){
						state = EXIBE_TELA3;
					}
				}

			}
			if(xSemaphoreTake(xSemaphoreCancel, 1000)){
				state = EXIBE_TELA5;
			}
			if (flag_rtc) {
				contador--;
				lv_label_set_text_fmt(label_valor_timer, "%02d s", contador);
				if (contador == 0) {
					troca_tela = 1;
					contador = 10;
				}
			}

			if (troca_tela) {
				troca_tela = 0;
				state = EXIBE_TELA1;
			}
			break;
			
			case EXIBE_TELA3:
			lv_page_3_pagamento();
			state = PAGAMENTO;
			break;
			
			case PAGAMENTO:
			send_package(payment_confirmed, 4);
			if(xQueueReceive(xQueueFeedback, &feedback, 1000)){
				if(feedback == 255){
					xSemaphoreGive(xSemaphorePago);
				}
				if(feedback == 2){
					state = EXIBE_TELA5;
				}
				if(feedback == 0){
					state = EXIBE_TELA1;
				}
			}

			if( xSemaphoreTake(xSemaphorePago, 1000) ){
				state = EXIBE_TELA4;
			}

			break;
			
			case EXIBE_TELA4:
			lv_page_4_preparando();
			state = PREPARANDO;
			break;
			
			case PREPARANDO:
			while (preparando) {
				uint16_t contador_cafe = lv_bar_get_value(bar_preparando);
				if (contador_cafe < 4)
				contador_cafe++;
				lv_bar_set_value(bar_preparando, contador_cafe, LV_ANIM_OFF);
				vTaskDelay(2000);
				if (contador_cafe == 4) {
					preparando = 0;
				}
			}
			vTaskDelay(3000);
			state = EXIBE_TELA1;
			break;
			
			case EXIBE_TELA5:
			lv_page_canceled();
			state = CANCELADO;
			break;

			case CANCELADO:
			if(xSemaphoreTake(xSemaphoreOk, 1000)){
				state = EXIBE_TELA1;
			}
			break;
			
			default:
			state = INICIAL;
			break;
		}
		
	}
}

/************************************************************************/
/* lvgl                                                                 */
/************************************************************************/
static void but_check(lv_obj_t * obj, lv_event_t event) {
	char p[] = {'U', 0, 0, 'X'};
	if(event == LV_EVENT_CLICKED)
	send_package(p, 4);
}

static void but_cobrar(lv_obj_t * obj, lv_event_t event) {
	char p[] = {'U', 1, 20, 'X'};
	if(event == LV_EVENT_CLICKED)
	send_package(p, 4);
}

static void but_verifica(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED)
	xSemaphoreGive(xSemaphoreVerifica);
}

static void handler_btn_expresso_1(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		int produto = 1;
		xQueueSend(xQueueProduto, &produto, 1000);
	}
}

static void handler_btn_longo_1(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		int produto = 2;
		xQueueSend(xQueueProduto, &produto, 1000);
	}
}

static void handler_btn_curto_1(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		int produto = 3;
		xQueueSend(xQueueProduto, &produto, 1000);
	}
}

static void handler_btn_pingado_1(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		int produto = 4;
		xQueueSend(xQueueProduto, &produto, 1000);
	}
}

static void handler_btn_ok_1(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		xSemaphoreGive(xSemaphoreOk);
	}
}

static void handler_btn_cancel(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {
		xSemaphoreGive(xSemaphoreCancel);
	}
}

static void handler_btn_plus_2(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {

		uint16_t regulagem = lv_bar_get_value(bar_regulagem);
		if (regulagem < 4)
		regulagem++;
		lv_bar_set_value(bar_regulagem, regulagem, LV_ANIM_OFF);
		lv_label_set_text_fmt(label_valor_regulagem, "%d", regulagem);
	}
}

static void handler_btn_minus_2(lv_obj_t * obj, lv_event_t event) {
	if(event == LV_EVENT_CLICKED) {

		uint16_t regulagem = lv_bar_get_value(bar_regulagem);
		if (regulagem > 0)
		regulagem--;
		lv_bar_set_value(bar_regulagem, regulagem, LV_ANIM_OFF);
		lv_label_set_text_fmt(label_valor_regulagem, "%d", regulagem);
	}
}

void lv_page_1_inicial(void){
	/*Cria uma pagina*/
	lv_obj_t * page_1 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_1, 320, 240);
	lv_obj_align(page_1, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	
	lv_obj_t * label_escolha;
	label_escolha = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_escolha, NULL, LV_ALIGN_CENTER, -75 , -90);
	lv_obj_set_style_local_text_font(label_escolha, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	lv_obj_set_style_local_text_color(label_escolha, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_escolha, "Escolha o produto");
	
	lv_obj_t * btn_expresso = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_expresso, handler_btn_expresso_1);
	lv_obj_set_width(btn_expresso, 120);  lv_obj_set_height(btn_expresso, 40);
	lv_obj_align(btn_expresso, NULL, LV_ALIGN_CENTER, -70, -20);
	lv_obj_t * label_btn_expresso;
	label_btn_expresso = lv_label_create(btn_expresso, NULL);
	lv_label_set_text(label_btn_expresso, "Expresso");
	
	lv_obj_t * btn_longo = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_longo, handler_btn_longo_1);
	lv_obj_set_width(btn_longo, 120);  lv_obj_set_height(btn_longo, 40);
	lv_obj_align(btn_longo, NULL, LV_ALIGN_CENTER, 70, -20);
	lv_obj_t * label_btn_longo;
	label_btn_longo = lv_label_create(btn_longo, NULL);
	lv_label_set_text(label_btn_longo, "Longo");
	
	lv_obj_t * btn_curto = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_curto, handler_btn_curto_1);
	lv_obj_set_width(btn_curto, 120);  lv_obj_set_height(btn_curto, 40);
	lv_obj_align(btn_curto, NULL, LV_ALIGN_CENTER, -70, 50);
	lv_obj_t * label_btn_curto;
	label_btn_curto = lv_label_create(btn_curto, NULL);
	lv_label_set_text(label_btn_curto, "Curto");
	
	lv_obj_t * btn_pingado = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_pingado, handler_btn_pingado_1);
	lv_obj_set_width(btn_pingado, 120);  lv_obj_set_height(btn_pingado, 40);
	lv_obj_align(btn_pingado, NULL, LV_ALIGN_CENTER, 70, 50);
	lv_obj_t * label_btn_pingado;
	label_btn_pingado = lv_label_create(btn_pingado, NULL);
	lv_label_set_text(label_btn_pingado, "Pingado");
}

void lv_page_2_configurando  (void){
	/*Cria uma pagina*/
	lv_obj_t * page_2 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_2, 320, 240);
	lv_obj_align(page_2, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	
	label_valor_timer = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_valor_timer, NULL, LV_ALIGN_CENTER, 5 , -25);
	lv_obj_set_style_local_text_font(label_valor_timer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
	lv_obj_set_style_local_text_color(label_valor_timer, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_RED);
	lv_label_set_text_fmt(label_valor_timer, "%02d s", 10);
	
	lv_obj_t * label_regulagem;
	label_regulagem = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_regulagem, NULL, LV_ALIGN_CENTER, -90 , -90);
	lv_obj_set_style_local_text_font(label_regulagem, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	lv_obj_set_style_local_text_color(label_regulagem, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_regulagem, "Regulagem de Acucar");
	
	lv_obj_t * btn_Plus = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_Plus, handler_btn_plus_2);
	lv_obj_set_width(btn_Plus, 40);  lv_obj_set_height(btn_Plus, 40);
	lv_obj_align(btn_Plus, NULL, LV_ALIGN_CENTER, 120 , 0);
	lv_obj_t * label_btn_Plus;
	label_btn_Plus = lv_label_create(btn_Plus, NULL);
	lv_label_set_recolor(label_btn_Plus, true);
	lv_label_set_text(label_btn_Plus, LV_SYMBOL_PLUS);
	
	lv_obj_t * btn_minus = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_minus, handler_btn_minus_2);
	lv_obj_set_width(btn_minus, 40);  lv_obj_set_height(btn_minus, 40);
	lv_obj_align(btn_minus, NULL, LV_ALIGN_CENTER, -120 , 0);
	lv_obj_t * label_btn_minus;
	label_btn_minus = lv_label_create(btn_minus, NULL);
	lv_label_set_recolor(label_btn_minus, true);
	lv_label_set_text(label_btn_minus, LV_SYMBOL_MINUS);

	bar_regulagem = lv_bar_create(lv_scr_act(), NULL);
	lv_obj_set_size(bar_regulagem, 175, 20);
	lv_bar_set_range(bar_regulagem, 0, 4);
	lv_obj_align(bar_regulagem, NULL, LV_ALIGN_CENTER, 0, 0);

	label_valor_regulagem = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_valor_regulagem, NULL, LV_ALIGN_CENTER, 5 , 20);
	lv_obj_set_style_local_text_font(label_valor_regulagem, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
	lv_obj_set_style_local_text_color(label_valor_regulagem, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_valor_regulagem, "0");
	
	lv_obj_t * btn_ok = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_ok, handler_btn_ok_1);
	lv_obj_set_width(btn_ok, 120);  lv_obj_set_height(btn_ok, 40);
	lv_obj_align(btn_ok, NULL, LV_ALIGN_CENTER, -65, 60);
	lv_obj_t * label_btn_ok;
	label_btn_ok = lv_label_create(btn_ok, NULL);
	lv_label_set_text(label_btn_ok, "OK");
	
	lv_obj_t * btn_cancel = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_cancel, handler_btn_cancel);
	lv_obj_set_width(btn_cancel, 120);  lv_obj_set_height(btn_cancel, 40);
	lv_obj_align(btn_cancel, NULL, LV_ALIGN_CENTER, 65, 60);
	lv_obj_t * label_btn_cancel;
	label_btn_cancel = lv_label_create(btn_cancel, NULL);
	lv_label_set_text(label_btn_cancel, "Cancel");
}

void lv_page_3_pagamento(void) {
	/*Cria uma pagina*/
	lv_obj_t * page_3 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_3, 320, 240);
	lv_obj_align(page_3, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);

	label_valor = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_valor, NULL, LV_ALIGN_CENTER, -50 , -80);
	lv_obj_set_style_local_text_font(label_valor, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_40);
	lv_obj_set_style_local_text_color(label_valor, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_RED);
	lv_label_set_text_fmt(label_valor, "R$ %d,00", price);
	
	label_aguardando = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_aguardando, NULL, LV_ALIGN_CENTER, -85 , 50);
	lv_obj_set_style_local_text_font(label_aguardando, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	lv_obj_set_style_local_text_color(label_aguardando, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_aguardando, "Aguardando Valor");
}

void lv_page_4_preparando(void) {
	/*Cria uma pagina*/
	lv_obj_t * page_4 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_4, 320, 240);
	lv_obj_align(page_4, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	
	lv_obj_t * img1 = lv_img_create(lv_scr_act(), NULL);
	lv_img_set_src(img1, &cafe_image);
	lv_obj_align(img1, NULL, LV_ALIGN_CENTER, 0, -20);
	
	lv_obj_t * label_preparando;
	label_preparando = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_preparando, NULL, LV_ALIGN_CENTER, -85 , 30);
	lv_obj_set_style_local_text_font(label_preparando, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	lv_obj_set_style_local_text_color(label_preparando, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_preparando, "Preparando cafe...");

	// animacao preparando café
	bar_preparando = lv_bar_create(lv_scr_act(), NULL);
	lv_obj_set_size(bar_preparando, 175, 20);
	lv_bar_set_range(bar_preparando, 0, 4);
	lv_obj_align(bar_preparando, NULL, LV_ALIGN_CENTER, 0, 60);
}

void lv_page_canceled(void){
	lv_obj_t * page_5 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_5, 320, 240);
	lv_obj_align(page_5, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	
	lv_obj_t * label_canceled;
	label_canceled = lv_label_create(lv_scr_act(), NULL);
	lv_obj_align(label_canceled, NULL, LV_ALIGN_IN_LEFT_MID, 30, 0);
	lv_obj_set_style_local_text_font(label_canceled, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_20);
	lv_obj_set_style_local_text_color(label_canceled, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
	lv_label_set_text_fmt(label_canceled, "PEDIDO CANCELADO!");
	
	lv_obj_t * btn_ok = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn_ok, handler_btn_ok_1);
	lv_obj_set_width(btn_ok, 120);  lv_obj_set_height(btn_ok, 40);
	lv_obj_align(btn_ok, NULL, LV_ALIGN_CENTER, 0, 60);
	lv_obj_t * label_btn_ok;
	label_btn_ok = lv_label_create(btn_ok, NULL);
	lv_label_set_text(label_btn_ok, "OK");
}

void lv_debug(void) {
	/*Cria uma pagina*/
	lv_obj_t * page_3 = lv_page_create(lv_scr_act(), NULL);
	lv_obj_set_size(page_3, 320, 240);
	lv_obj_align(page_3, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	
	lv_obj_t * label;
	lv_obj_t * btn1 = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn1, but_check);
	lv_obj_align(btn1, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	label = lv_label_create(btn1, NULL);
	lv_label_set_text(label, "Check");

	lv_obj_t * btn2 = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn2, but_cobrar);
	lv_obj_align(btn2, btn1, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	label = lv_label_create(btn2, NULL);
	lv_label_set_text(label, "Cobrar");
	
	lv_obj_t * btn3 = lv_btn_create(lv_scr_act(), NULL);
	lv_obj_set_event_cb(btn3, but_verifica);
	lv_obj_align(btn3, btn2, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
	label = lv_label_create(btn3, NULL);
	lv_label_set_text(label, "Verifica");
	
	labelDebug = lv_label_create(lv_scr_act(), NULL);
	lv_obj_set_width(labelDebug, 150);
	lv_obj_align(labelDebug, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, -30);
}
/************************************************************************/
/* funcs                                                               */
/************************************************************************/
static void send_package(char data[], char n) {
	for (int i = 0; i < n; i++){
		while(!usart_is_tx_ready(CONSOLE_UART)) {
			vTaskDelay(10 / portTICK_PERIOD_MS);
		}
		usart_write(CONSOLE_UART, data[i]);
	}
}

static void configure_lcd(void) {
	/**LCD pin configure on SPI*/
	pio_configure_pin(LCD_SPI_MISO_PIO, LCD_SPI_MISO_FLAGS);  //
	pio_configure_pin(LCD_SPI_MOSI_PIO, LCD_SPI_MOSI_FLAGS);
	pio_configure_pin(LCD_SPI_SPCK_PIO, LCD_SPI_SPCK_FLAGS);
	pio_configure_pin(LCD_SPI_NPCS_PIO, LCD_SPI_NPCS_FLAGS);
	pio_configure_pin(LCD_SPI_RESET_PIO, LCD_SPI_RESET_FLAGS);
	pio_configure_pin(LCD_SPI_CDS_PIO, LCD_SPI_CDS_FLAGS);
	
}

static void configure_console(void) {
	const usart_serial_options_t uart_serial_options = {
		.baudrate = USART_SERIAL_EXAMPLE_BAUDRATE,
		.charlength = USART_SERIAL_CHAR_LENGTH,
		.paritytype = USART_SERIAL_PARITY,
		.stopbits = USART_SERIAL_STOP_BIT,
	};

	/* Configure console UART. */
	stdio_serial_init(CONSOLE_UART, &uart_serial_options);

	/* Specify that stdout should not be buffered. */
	setbuf(stdout, NULL);
	
	/* ativando interrupcao */
	usart_enable_interrupt(CONSOLE_UART, US_IER_RXRDY);
	NVIC_SetPriority(CONSOLE_UART_ID, 4);
	NVIC_EnableIRQ(CONSOLE_UART_ID);
}

static void USART1_init(void){
	/* Configura USART1 Pinos */
	sysclk_enable_peripheral_clock(ID_PIOB);
	sysclk_enable_peripheral_clock(ID_PIOA);
	pio_set_peripheral(PIOB, PIO_PERIPH_D, PIO_PB4); // RX
	pio_set_peripheral(PIOA, PIO_PERIPH_A, PIO_PA21); // TX
	MATRIX->CCFG_SYSIO |= CCFG_SYSIO_SYSIO4;

	/* Configura opcoes USART */
	const sam_usart_opt_t usart_settings = {
		.baudrate       = 115200,
		.char_length    = US_MR_CHRL_8_BIT,
		.parity_type    = US_MR_PAR_NO,
		.stop_bits   	= US_MR_NBSTOP_1_BIT	,
		.channel_mode   = US_MR_CHMODE_NORMAL
	};

	/* Ativa Clock periferico USART0 */
	sysclk_enable_peripheral_clock(CONSOLE_UART_ID);

	/* Configura USART para operar em modo RS232 */
	usart_init_rs232(CONSOLE_UART, &usart_settings, sysclk_get_peripheral_hz());

	/* Enable the receiver and transmitter. */
	usart_enable_tx(CONSOLE_UART);
	usart_enable_rx(CONSOLE_UART);

	/* map printf to usart */
	ptr_put = (int (*)(void volatile*,char))&usart_serial_putchar;
	ptr_get = (void (*)(void volatile*,char*))&usart_serial_getchar;

	/* ativando interrupcao */
	usart_enable_interrupt(CONSOLE_UART, US_IER_RXRDY);
	NVIC_SetPriority(CONSOLE_UART_ID, 4);
	NVIC_EnableIRQ(CONSOLE_UART_ID);
}

/************************************************************************/
/* port lvgl                                                            */
/************************************************************************/

void my_flush_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p) {
	ili9341_set_top_left_limit(area->x1, area->y1);   ili9341_set_bottom_right_limit(area->x2, area->y2);
	ili9341_copy_pixels_to_screen(color_p,  (area->x2 - area->x1) * (area->y2 - area->y1));
	
	lv_disp_flush_ready(disp_drv);
}

bool my_input_read(lv_indev_drv_t * drv, lv_indev_data_t*data) {
	int px, py, pressed;
	
	if (readPoint(&px, &py)) {
		data->state = LV_INDEV_STATE_PR;
	}
	else {
		data->state = LV_INDEV_STATE_REL;
	}
	
	data->point.x = px;
	data->point.y = py;
	return false; /*No buffering now so no more data read*/
}

void init(void){
	pmc_enable_periph_clk(LED_PIO_ID);

	pio_configure(LED_PIO, PIO_OUTPUT_0, LED_IDX_MASK, PIO_DEBOUNCE);
}

/************************************************************************/
/* main                                                                 */
/************************************************************************/
int main(void) {
	
	xQueueRx = xQueueCreate(32, sizeof(char));
	xQueueFeedback = xQueueCreate(32, sizeof(char));
	
	/* board and sys init */
	board_init();
	sysclk_init();
	USART1_init();
	init();
	
	/*RTC*/
	currentTime = (calendar) {2018, 3, 19, 12, 03, 04, 0};
	RTC_init(RTC, ID_RTC, currentTime, RTC_IER_ALREN);
	
	/* LCd int */
	configure_lcd();
	ili9341_init();
	configure_touch();
	ili9341_backlight_on();
	
	/*LittlevGL init*/
	lv_init();
	lv_disp_drv_t disp_drv;                 /*A variable to hold the drivers. Can be local variable*/
	lv_disp_drv_init(&disp_drv);            /*Basic initialization*/
	lv_disp_buf_init(&disp_buf, buf_1, NULL, LV_HOR_RES_MAX * LV_VER_RES_MAX);  /*Initialize `disp_buf` with the buffer(s) */
	disp_drv.buffer = &disp_buf;            /*Set an initialized buffer*/
	disp_drv.flush_cb = my_flush_cb;        /*Set a flush callback to draw to the display*/
	lv_disp_t * disp;
	disp = lv_disp_drv_register(&disp_drv); /*Register the driver and save the created display objects*/
	
	/* Init input on LVGL */
	lv_indev_drv_t indev_drv;
	lv_indev_drv_init(&indev_drv);      /*Basic initialization*/
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.read_cb = my_input_read;
	/*Register the driver in LVGL and save the created input device object*/
	lv_indev_t * my_indev = lv_indev_drv_register(&indev_drv);

	if (xTaskCreate(task_lcd, "LCD", TASK_LCD_STACK_SIZE, NULL, TASK_LCD_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create lcd task\r\n");
	}
	
	if (xTaskCreate(task_main, "main", TASK_APS2_STACK_SIZE, NULL, TASK_APS2_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create Main task\r\n");
	}
	
	if (xTaskCreate(task_receive, "receive", TASK_APS2_STACK_SIZE, NULL, TASK_APS2_STACK_PRIORITY, NULL) != pdPASS) {
		printf("Failed to create Main task\r\n");
	}
	
	/* Start the scheduler. */
	vTaskStartScheduler();

	/* RTOS n?o deve chegar aqui !! */
	while(1){ }
}
