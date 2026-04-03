#include "CAN_send.h"
#include "CAN_receive.h"
#include "can.h"

/*
   Ö÷¿ØCan·¢ËÍº¯Êý
*/
//extern CAN_HandleTypeDef hcan1;


uint8_t Board_Tx(uint32_t std, uint8_t *data,char hcan1,uint32_t DL)
{
	uint8_t i;
	CAN_TxHeaderTypeDef TxMes_header;
	uint8_t TxMes_data[8];
	uint32_t CAN_Tx_Mail_Box;

	TxMes_header.IDE = CAN_ID_STD;
	TxMes_header.RTR = CAN_RTR_DATA;
	TxMes_header.DLC = DL;    //
	TxMes_header.StdId = std; 	  //
	
	TxMes_data[0] = data[0];
	TxMes_data[1] = data[1];	
	TxMes_data[2] = data[2];	
	TxMes_data[3] = data[3];

	TxMes_data[4] = data[4];
	TxMes_data[5] = data[5];	
	TxMes_data[6] = data[6];	
	TxMes_data[7] = data[7];
	
	if(hcan1 == 1)i = HAL_CAN_AddTxMessage(&hcan,&TxMes_header,TxMes_data,  &CAN_Tx_Mail_Box);
//	i = HAL_CAN_AddTxMessage(&hcan,&TxMes_header,TxMes_data,&CAN_Tx_Mail_Box);
//	if(hcan == &hcan1)
//	{
//		i = HAL_CAN_AddTxMessage(hcan, &TxMes_header, TxMes_data, &CAN_Tx_Mail_Box);
//	}
	return i;
}



























