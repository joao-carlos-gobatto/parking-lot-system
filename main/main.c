#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "time.h"
#include "esp_log.h"
#include "config.h"
#include "types.h"
#include "utils.h"
#include "print.h"

#define ESP_INTR_FLAG_DEFAULT 0

static const char *MAIN_TAG = "PARKING_SYSTEM";

uint16_t globalSystemTime;    //Global time.

static QueueHandle_t gpio_evt_queue = NULL;
static QueueHandle_t entrance_queue = NULL;
static QueueHandle_t exit_queue = NULL;

SemaphoreHandle_t parking_lot_semaphore;

uint16_t total_cars = 0;
uint8_t parked_cars = 0;
uint32_t total_money = 0;
car_t car_in_entrance_gate, car_in_exit_gate;
receipt_t receipt;

const parking_spot_t empty_parking_slot = {
    .status = EMPTY,
    .parked_car = {
        .time_parked = 0,
        .plate = {32,32,32,32,32,32,32},
    },
};

const car_t empty_car = {
    .parking_spot = -1,
    .entrance_time = 0,
    .time_parked = 0,
    .plate = {32,32,32,32,32,32,32},
};

parking_spot_t parking_lot[PARKING_LOT_CAPACITY];

// Interrupt service routine for the GPIO pin
void IRAM_ATTR buttonISRHandler(void* arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    if(gpio_evt_queue)
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void carTask(void *arg){
    uint8_t in_parking_slot = 1;
    car_t *p_car = (car_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(p_car->time_parked));
    while (in_parking_slot)
    {
        if(pdTRUE == xQueueSend(exit_queue, p_car, pdMS_TO_TICKS(CAR_SPEED))){
            in_parking_slot = 0;
            xSemaphoreTake(parking_lot_semaphore, portMAX_DELAY);
            memcpy(&parking_lot[p_car->parking_spot], &empty_parking_slot, sizeof(parking_spot_t));
            xSemaphoreGive(parking_lot_semaphore);
        }
    }
    vTaskDelete(NULL);
}

static void parkingLotTask(void *arg){
    while (true)
    {
        parked_cars = 0;
        xSemaphoreTake(parking_lot_semaphore, portMAX_DELAY);
        for (size_t i = 0; i < PARKING_LOT_CAPACITY; i++)
        {
            if(parking_lot[i].status == OCCUPIED){
                parked_cars++;
            }
        }
        xSemaphoreGive(parking_lot_semaphore);
        globalSystemTime = time(NULL);
        vTaskDelay(pdMS_TO_TICKS(PRINT_DELAY));
    }
}

static void gpioReadTask(void* arg){
    uint32_t io_num;
    while(true) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            gpio_isr_handler_remove(CAR_ENTERED_BTN);
            car_t new_car;
            initiateCar(&new_car);
            xSemaphoreTake(parking_lot_semaphore, portMAX_DELAY);
            uint8_t empty_parking_spots = countFreeParkingSlots(parking_lot, PARKING_LOT_CAPACITY);
            xSemaphoreGive(parking_lot_semaphore);
            if(0 < empty_parking_spots){
                if(!isQueueFull(entrance_queue)){
                    xQueueSend(entrance_queue, &new_car, pdMS_TO_TICKS(CAR_SPEED));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCING_TIME)); //Button press debouncing treatment.
            gpio_isr_handler_add(CAR_ENTERED_BTN, buttonISRHandler, (void*) CAR_ENTERED_BTN);
        }
    }
}

static void entranceGateTask(void* arg){
    while(true) {
        if(xQueueReceive(entrance_queue, &car_in_entrance_gate, portMAX_DELAY)) {
            car_in_entrance_gate.entrance_time = time(NULL);
            vTaskDelay(pdMS_TO_TICKS(GATE_OPEN_TIME));
            xSemaphoreTake(parking_lot_semaphore, portMAX_DELAY);
            int8_t parking_area_number = findFirstEmptyParkingSlotIndex(parking_lot, PARKING_LOT_CAPACITY);
            car_in_entrance_gate.parking_spot = parking_area_number;
            parking_lot[parking_area_number].status = OCCUPIED;
            memcpy(&parking_lot[parking_area_number].parked_car, &car_in_entrance_gate, sizeof(car_t));
            total_cars++;
            xSemaphoreGive(parking_lot_semaphore);
            xTaskCreate(carTask, "carTask", 2048, &parking_lot[parking_area_number].parked_car, 2, NULL);
            memcpy(&car_in_entrance_gate, &empty_car, sizeof(car_t));
            vTaskDelay(pdMS_TO_TICKS(GATE_CLOSE_TIME));
        }
    }
}

static void exitGateTask(void* arg){
    while(true) {
        if(xQueueReceive(exit_queue, &car_in_exit_gate, portMAX_DELAY)) {
            receipt.entrance_time = car_in_exit_gate.entrance_time + (GATE_OPEN_TIME%1000);
            receipt.exit_time = time(NULL);
            memcpy(receipt.car_plate, car_in_exit_gate.plate,CAR_PLATE_LENGTH*sizeof(uint8_t));
            receipt.total_time = receipt.exit_time - receipt.entrance_time;
            receipt.value = calculateParkingFee(car_in_exit_gate.entrance_time, receipt.exit_time, TIME_FEE);
            total_money += receipt.value;
            vTaskDelay(pdMS_TO_TICKS(GATE_OPEN_TIME));
            memcpy(&car_in_exit_gate, &empty_car, sizeof(car_t));
            vTaskDelay(pdMS_TO_TICKS(GATE_CLOSE_TIME));
        }
    }
}

static void systemPrintTask(void* arg){
    uint8_t cars_in_entrance_queue = 0;
    uint8_t cars_in_exit_queue = 0;
    uint8_t empty_car_plate[CAR_PLATE_LENGTH];
    for (size_t i = 0; i < CAR_PLATE_LENGTH-1; i++)
    {
        empty_car_plate[i] = 32;
    }
    printParkingLot(25,1);
    while (true)
    {
        cars_in_entrance_queue = QUEUES_MAX_SIZE - uxQueueSpacesAvailable(entrance_queue);
        cars_in_exit_queue = QUEUES_MAX_SIZE - uxQueueSpacesAvailable(exit_queue);

        uint8_t entrance_gate_is_empty = !(memcmp(car_in_entrance_gate.plate,empty_car_plate,CAR_PLATE_LENGTH*sizeof(uint8_t))!= 0);
        uint8_t exit_gate_is_empty = !(memcmp(car_in_exit_gate.plate,empty_car_plate,CAR_PLATE_LENGTH*sizeof(uint8_t))!= 0);
        for (size_t i = 0; i < PARKING_LOT_CAPACITY; i++)
        {
            int row = i % 5;
            int col = i / 5;
            uint8_t is_empty = (EMPTY == parking_lot[i].status);
            printParkingSpot(
                27+(row*9),
                3+(col*3),
                parking_lot[i].parked_car.plate,
                is_empty
            );
        }
        printSystemTable(
            75,2,
            total_cars,
            parked_cars,
            PARKING_LOT_CAPACITY,
            car_in_entrance_gate.plate,
            car_in_exit_gate.plate,
            globalSystemTime,
            total_money,
            cars_in_entrance_queue,
            cars_in_exit_queue,
            receipt.car_plate,
            receipt.total_time,
            receipt.value
        );

        printGate(0,1,"   ENTRACE GATE   ", car_in_entrance_gate.plate, entrance_gate_is_empty);
        printGate(0,12,"     EXIT GATE    ", car_in_exit_gate.plate, exit_gate_is_empty);
        gotoxy(0,20);
        vTaskDelay(pdMS_TO_TICKS(PRINT_DELAY)); //Button press debouncing treatment.
    }
}

void app_main(void)
{
    gpio_set_direction(CAR_ENTERED_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CAR_ENTERED_BTN, GPIO_PULLUP_ONLY);

    //change gpio interrupt type for one pin
    gpio_set_intr_type(CAR_ENTERED_BTN, GPIO_INTR_NEGEDGE);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(CAR_ENTERED_BTN, buttonISRHandler, (void*) CAR_ENTERED_BTN);


    parking_lot_semaphore = xSemaphoreCreateMutex();
    if (parking_lot_semaphore == NULL) {
        ESP_LOGE(MAIN_TAG, "Failed to create parking lot semaphore");
        exit(1);
    }
    xSemaphoreGive(parking_lot_semaphore);

    memcpy(&car_in_entrance_gate, &empty_car, sizeof(car_t));
    memcpy(&car_in_exit_gate, &empty_car, sizeof(car_t));
    clrscr();

    gpio_evt_queue = xQueueCreate(QUEUES_MAX_SIZE, sizeof(uint32_t));
    entrance_queue = xQueueCreate(QUEUES_MAX_SIZE, sizeof(car_t));
    exit_queue = xQueueCreate(QUEUES_MAX_SIZE, sizeof(car_t));

    xTaskCreate(gpioReadTask, "gpioReadTask", 2048, NULL, 2, NULL);
    xTaskCreate(entranceGateTask, "entranceGateTask", 2048, NULL, 3, NULL);
    xTaskCreate(parkingLotTask, "parkingLotTask", 2048, NULL, 4, NULL);
    xTaskCreate(exitGateTask, "exitGateTask", 2048, NULL, 3, NULL);
    xTaskCreate(systemPrintTask, "systemPrintTask", 2048, NULL, 10, NULL);
}