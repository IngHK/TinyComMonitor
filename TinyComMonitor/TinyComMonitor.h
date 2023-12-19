/**
 ******************************************************************************
 * @file           : TinyComMonitor.h
 * @brief          : Tiny COM Monitor main
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 IngHK Ingenieurbuero Heiko Kuester
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the same directory of this software component.
 *
 ******************************************************************************
 */

#ifndef TINYCOMMONITOR_H_
#define TINYCOMMONITOR_H_

#include <stdint.h>
#include "stm32f4xx.h"

extern uint8_t UsbCdcRxByte;

void ClearUsbCdcVcomReady ( void );
void TinyComMonitorUsartIrqHandler ( USART_TypeDef *Usart );
void TinyComMonitor ( void );

#endif /* TINYCOMMONITOR_H_ */
