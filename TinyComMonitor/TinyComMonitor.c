/**
 ******************************************************************************
 * @file           : TinyComMonitor.c
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

#include <stdbool.h>
#include <string.h>
#include "usbd_cdc_if.h"
#include "TinyComMonitor.h"

#define TinyComMonitorVersion         "V1.0"

#define EscSequenceReset              "\x1b[0m"
#define EscSequenceInverse            "\x1b[7m"
#define EscSequenceYellow             "\x1b[33m"
#define EscSequenceRedInverse         "\x1b[31;7m"
#define EscSequenceCom0               EscSequenceReset                                                                // 1st COM port outputs
#define EscSequenceCom1               EscSequenceInverse                                                              // 2nd COM port outputs
#define EscSequenceTCM                EscSequenceReset EscSequenceYellow                                              // TinyComMonitor outputs
const uint8_t                         *EscSequenceCom[2] = { (uint8_t*)EscSequenceCom0, (uint8_t*)EscSequenceCom1 };  // put EscSequenceCom sequences in array

#ifdef DEBUG
#define DebugMessage                  " " EscSequenceInverse "(debug build)" EscSequenceTCM
#else
#define DebugMessage                  ""
#endif
#define StartupMessage                ( EscSequenceReset EscSequenceTCM "\x0c\rTinyComMonitor " TinyComMonitorVersion DebugMessage "\r\n(c) IngHK Ingenieurbuero Heiko Kuester\r\n" EscSequenceReset )

#define WaitTicksAfterCdcVcomReady    100                                                                             // wait time [ms] after USB virtual COM is configured by PC with terminal program
uint32_t                              UsbCdcChangeTimestamp = 0;                                                      // timestamp used for wait time after USB virtual COM is configured

#define ARRAY_LEN(ARRAY_NAME)         ( sizeof ( ARRAY_NAME ) / sizeof ( ARRAY_NAME[0] ) )

const uint8_t                         OverflowMessage[] = ( EscSequenceRedInverse " " EscSequenceReset );             // inverse red space
const uint8_t                         OverflowMessageLength = strlen ( (char*)OverflowMessage );

#define nUsart                        2                                                                               // COM port quantity
const USART_TypeDef                   *Usarts[nUsart] = { USART1, USART2 };                                           // array of COM ports
LL_USART_InitTypeDef                  UsartInitStruct = {0};                                                          // init structures for COM ports
USART_TypeDef                         *LastUsart = NULL;                                                              // flag for last COM port with byte reception

bool                                  UsbCdcVcomReady = false;                                                           // virtual USB COM port is ready. not ready if com config changed

#define UsbCdcTxBufferSize            APP_TX_DATA_SIZE                                                                /* size of send double buffer via USB CDC virtual COM ports */
uint8_t                               UsbCdcTxBuffer[2][UsbCdcTxBufferSize];                                          // send double buffer via USB CDC virtual COM ports
uint32_t                              UsbCdcTxBufferFillLevel[2]  = { 0, 0 };                                         // fill levels of both buffers
uint8_t                               UsbCdcTxBufferFillIndex     = 0;                                                // pointer to current buffer to be filled
#define                               OppositeBuffer(BufferIndex) ( 1 - BufferIndex )

uint8_t                               UsbCdcRxByte = 0;                                                               // received byte (used for command) via USB CDC virtual COM port

uint32_t                              BaudrateList[] = { 600, 1200, 1800, 2400, 4800, 9600, 19200, 28800, 38400, 57600, 76800, 115200, 230400, 460800, 576000, 921600 };
uint8_t                               BaudrateIndex  = 5;                                                             // index of startup baudrate: 9600

bool                                  ImplicitLF = false;                                                             // flags to add implicit LF after CR
bool                                  ImplicitCR = false;                                                             // flags to add implicit CR after LF


// function to add byte array or string to USB CDC virtual COM buffer to be send to terminal PC at the next opportunity
// it uses double buffer UsbCdcTxBuffer to avoid conflicts between USART COM receive and USB CDC virtual COM send
// in case of buffer overflow the OverflowMessage is added
bool AppendToUsbCdcTxBuffer ( uint8_t *buf, uint32_t len )
{
  bool    result                  = true;

  result &= ( buf != NULL );                                                                                // check buf
  if ( result )
  {
    if ( len == 0 )                                                                                         // if len = 0 analyze buf as string
      len = strlen ( (char*)buf );
    result &= ( len != 0 );                                                                                 // fail if len is still 0
    if ( result )
    {
      __disable_irq();                                                                                      // begin critical path
      if ( UsbCdcTxBufferFillLevel [ OppositeBuffer ( UsbCdcTxBufferFillIndex ) ] == 0 )                    // different handling if empty send buffer (swap possible)
      {                                                                                                     // send buffer is empty
        result &= ( ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] + len ) <= UsbCdcTxBufferSize );  // enough space left to append buf?
        if ( !result )                                                                                      // if fill buffer is full
        {
          UsbCdcTxBufferFillIndex = OppositeBuffer ( UsbCdcTxBufferFillIndex );                             // then swap fill and send buffers
          result |= ( ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] + len ) <= UsbCdcTxBufferSize );
        }
      }
      else                                                                                                  // else send buffer is NOT empty (swap not possible)
      {
        result &= ( ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] + len ) <=                        // enough space left to append buf and maybe OverflowMessageLength?
            ( UsbCdcTxBufferSize - OverflowMessageLength ) );
        if ( !result )                                                                                      // if fill buffer is full, try to add OverflowMessage
        {
          buf = (uint8_t*)OverflowMessage;                                                                  // add Overflow message
          len = OverflowMessageLength;
          result |= ( ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] + len ) <=                      // enough space left to append OverflowMessage?
              UsbCdcTxBufferSize );
        }
      }
      if ( result )
      {
        for ( uint32_t i = 0; i < len; i++ )                                                                // if buffer has free space, fill it with buf data byte-by-byte
          UsbCdcTxBuffer [ UsbCdcTxBufferFillIndex ] [ UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] + i ] = buf [ i ];
        UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] += len;
      }
      __enable_irq();                                                                                       // end critical path
    }
  }
  return result;
}


// function to send UsbCdcTxBuffer via USB CDC virtual COM to terminal PC if possible
bool SendUsbCdcTxBuffer ( void )
{
  bool    result                  = true;
  uint8_t UsbCdcTxBufferSendIndex = 0;

  result &= ( CDC_Transmit_FS ( NULL, 0 ) == USBD_OK );                                                   // USB CDC ready to send?
  if ( result )
  {
    __disable_irq();                                                                                      // begin critical path
    UsbCdcTxBufferSendIndex = OppositeBuffer ( UsbCdcTxBufferFillIndex );
    result &= ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferSendIndex ] > 0 );                                // send buffer filled?
    if ( !result )
    {
      result |= ( UsbCdcTxBufferFillLevel [ UsbCdcTxBufferFillIndex ] > 0 );                              // if no: fill buffer filled?
      if ( result )
      {
        UsbCdcTxBufferFillIndex = OppositeBuffer ( UsbCdcTxBufferFillIndex );                             // then swap fill and send buffers
        UsbCdcTxBufferSendIndex = OppositeBuffer ( UsbCdcTxBufferFillIndex );
      }
    }
    __enable_irq();                                                                                       // end critical path
    if ( result )                                                                                         // send buffer via USB CDC
    {
      result &= ( CDC_Transmit_FS ( UsbCdcTxBuffer [ UsbCdcTxBufferSendIndex ], UsbCdcTxBufferFillLevel [ UsbCdcTxBufferSendIndex ] ) == USBD_OK );
      if ( result )
        UsbCdcTxBufferFillLevel [ UsbCdcTxBufferSendIndex ] = 0;                                          // and clear send buffer
    }
  }
  return result;
}


// function to handle received USART COM bytes
void TinyComMonitorUsartIrqHandler ( USART_TypeDef *Usart )
{
  uint8_t UsartRxByte = 0;

  if ( LL_USART_IsActiveFlag_RXNE ( Usart ) )
  {
    if ( LastUsart != Usart )
      AppendToUsbCdcTxBuffer ( (uint8_t*)EscSequenceCom [ Usart == Usarts[0] ? 0 : 1 ], 0 );
    while ( LL_USART_IsActiveFlag_RXNE ( Usart ) )
    {
      UsartRxByte = LL_USART_ReceiveData8 ( Usart );
      AppendToUsbCdcTxBuffer ( &UsartRxByte, 1 );
      if ( UsartRxByte == '\r' && ImplicitLF )
        AppendToUsbCdcTxBuffer ( (uint8_t*)"\n", 1 );
      if ( UsartRxByte == '\n' && ImplicitCR )
        AppendToUsbCdcTxBuffer ( (uint8_t*)"\r", 1 );
    }
    LastUsart = Usart;
  }
  LL_USART_ClearFlag_ORE ( Usart );
}


// function handle received bytes via USB CDC virtual COM buffer from terminal PC
uint8_t GetUsbCdcRxByte ( void )
{
  uint8_t result = 0;

  if ( UsbCdcRxByte != 0 ) // no critical path
  {
    result = UsbCdcRxByte;
    UsbCdcRxByte = 0;
  }
  return result;
}


// function to clear/reset UsbCdcVcomReady flag
// UsbCdcVcomReady signals a reconnected or reconfigures USB CDC virtual COM port
void ClearUsbCdcVcomReady ( void )
{
  UsbCdcChangeTimestamp = HAL_GetTick();
  UsbCdcVcomReady          = false;
}


// function to output current settings and status via USB CDC virtual COM
bool PrintSettingsStatusToUsbCdcBuffer ( void )
{
  bool    result = true;
  uint8_t PrintBuffer[UsbCdcTxBufferSize];

  sprintf (
      (char*)PrintBuffer,
      EscSequenceTCM "Settings: %lu Bd, %c%c%c, implicit LF:%s CR:%s\r\n" EscSequenceReset,
      UsartInitStruct.BaudRate,
      ( UsartInitStruct.DataWidth == LL_USART_DATAWIDTH_8B ? '8' : ( UsartInitStruct.DataWidth == LL_USART_DATAWIDTH_9B ? '9' : '?' ) ),
      ( UsartInitStruct.Parity    == LL_USART_PARITY_NONE  ? 'N' : ( UsartInitStruct.Parity    == LL_USART_PARITY_EVEN  ? 'E' : ( UsartInitStruct.Parity == LL_USART_PARITY_ODD ? 'O' : '?' ) ) ),
      ( UsartInitStruct.StopBits  == LL_USART_STOPBITS_1   ? '1' : ( UsartInitStruct.StopBits  == LL_USART_STOPBITS_2   ? '2' : '?' ) ),
      ( ImplicitLF ? "on" : "off" ),
      ( ImplicitCR ? "on" : "off" ) );
  result &= AppendToUsbCdcTxBuffer ( PrintBuffer, 0 );
  LastUsart = NULL;
  return result;
}


// function to init/reinit USART COMs due to startup or changed settings
bool ReInitComs ( void )
{
  bool result = true;

  UsartInitStruct.BaudRate = BaudrateList [ BaudrateIndex ];
  for ( uint8_t h = 0; h < nUsart && result; h++ )
  {
    result &= ( LL_USART_DeInit ( (USART_TypeDef*)Usarts[h] ) == SUCCESS );
    if ( result )
      result &= ( LL_USART_Init ( (USART_TypeDef*)Usarts[h], &UsartInitStruct ) == SUCCESS );
    if ( result )
    {
      LL_USART_ConfigAsyncMode ( (USART_TypeDef*)Usarts[h] );
      LL_USART_Enable ( (USART_TypeDef*)Usarts[h] );
      LL_USART_EnableIT_RXNE ( (USART_TypeDef*)Usarts[h] );
    }
  }
  if ( result )
    result &= PrintSettingsStatusToUsbCdcBuffer();
  return result;
}


// function to set USART parity
bool SetParity ( uint32_t Parity )
{
  bool result = true;

  result &= ( UsartInitStruct.Parity != Parity );
  if ( result )
    UsartInitStruct.Parity = Parity;
  return result;
}


// function to set USART stop bit quantity
bool SetStopBits ( uint32_t StopBits )
{
  bool result = true;

  result &= ( UsartInitStruct.StopBits != StopBits );
  if ( result )
    UsartInitStruct.StopBits = StopBits;
  return result;
}


// function to output help via USB CDC virtual COM
bool PrintHelpToUsbCdcBuffer ( void )
{
  bool    result = true;
  uint8_t PrintBuffer[UsbCdcTxBufferSize];

  sprintf ( (char*)PrintBuffer,
      EscSequenceTCM
      "+/-   ... increase/decrease baudrate\r\n"
      "n/o/e ... set parity to none, odd resp. even\r\n"
      "1/2   ... set stop bit quantity to 1 resp. 2\r\n"
      "l/c   ... implicit LF after CR resp. CR after LF\r\n"
      "s     ... show settings & status\r\n"
      "h/?   ... help\r\n"
      EscSequenceReset );
  result &= AppendToUsbCdcTxBuffer ( PrintBuffer, 0 );
  LastUsart = NULL;
  return result;
}


// function to handle command inputs via USB CDC virtual COM
bool UsbCdcCommand ( uint8_t command )
{
  bool result     = true;
  bool reinit     = false;
  bool showstatus = false;

  result &= ( command != 0 );
  if ( result )
  {
    switch ( command )
    {
    case '+':
      result &= ( BaudrateIndex < ( ARRAY_LEN ( BaudrateList ) - 1 ) );
      if ( result )
        BaudrateIndex++;
      reinit = result;
      break;
    case '-':
      result &= ( BaudrateIndex > 0 );
      if ( result )
        BaudrateIndex--;
      reinit = result;
      break;
    case 'n':
      result &= SetParity ( LL_USART_PARITY_NONE );
      reinit = result;
      break;
    case 'o':
      result &= SetParity ( LL_USART_PARITY_ODD );
      reinit = result;
      break;
    case 'e':
      result &= SetParity ( LL_USART_PARITY_EVEN );
      reinit = result;
      break;
    case '1':
      result &= SetStopBits ( LL_USART_STOPBITS_1 );
      reinit = result;
      break;
    case '2':
      result &= SetStopBits ( LL_USART_STOPBITS_2 );
      reinit = result;
      break;
    case 'l':
      ImplicitLF = !ImplicitLF;
      showstatus = true;
      break;
    case 'c':
      ImplicitCR = !ImplicitCR;
      showstatus = true;
      break;
    case 's':
      result &= AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
      showstatus = true;
      break;
    default: // also h and ?
      result &= AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
      result &= PrintHelpToUsbCdcBuffer();
      break;
    }
    if ( reinit && result )
    {
      result &= AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
      if ( result )
        result &= ReInitComs();
    }
    if ( showstatus && result )
      result &= PrintSettingsStatusToUsbCdcBuffer();
  }
  return result;
}


// the main function
void TinyComMonitor ( void )
{
  LL_USART_StructInit ( &UsartInitStruct );
  ReInitComs();
  while ( 1 )
  {
    if ( !UsbCdcInit )                                                                    // USB CDC device ready?
    {
      UsbCdcTxBufferFillLevel[0] = 0;                                                     // clear USB CDC send buffers
      UsbCdcTxBufferFillLevel[1] = 0;
    }
    else if ( !UsbCdcVcomReady )                                                          // virtual USB COM port ready?
    {
      if ( ( HAL_GetTick() - UsbCdcChangeTimestamp ) > WaitTicksAfterCdcVcomReady )       // send StartupMessage (again) after WaitTicksAfterCdcVcomReady time
      {
        AppendToUsbCdcTxBuffer ( (uint8_t*)StartupMessage, 0 );
        AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
        PrintHelpToUsbCdcBuffer();
        AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
        PrintSettingsStatusToUsbCdcBuffer();
        AppendToUsbCdcTxBuffer ( (uint8_t*)"\r\n", 0 );
        UsbCdcVcomReady = true;
      }
    }
    else                                                                                  // running: endless loop
    {
      SendUsbCdcTxBuffer();
      UsbCdcCommand ( GetUsbCdcRxByte() );
    }
  }
}
