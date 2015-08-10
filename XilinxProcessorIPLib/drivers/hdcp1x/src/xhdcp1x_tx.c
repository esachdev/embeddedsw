/******************************************************************************
*
* Copyright (C) 2015 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/
/*****************************************************************************/
/**
*
* @file xhdcp1x_tx.c
*
* This contains the main implementation file for the Xilinx HDCP transmit
* state machine
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who    Date     Changes
* ----- ------ -------- --------------------------------------------------
* 1.00  fidus  07/16/15 Initial release.
* </pre>
*
*****************************************************************************/

/***************************** Include Files *********************************/

#include "xparameters.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xhdcp1x.h"
#include "xhdcp1x_cipher.h"
#include "xhdcp1x_debug.h"
#include "xhdcp1x_platform.h"
#include "xhdcp1x_port.h"
#if defined(XPAR_XHDMI_TX_NUM_INSTANCES) && (XPAR_XHDMI_TX_NUM_INSTANCES > 0)
#include "xhdcp1x_port_hdmi.h"
#else
#include "xhdcp1x_port_dp.h"
#endif
#include "xhdcp1x_tx.h"
#include "xil_types.h"

/************************** Constant Definitions *****************************/

#define XVPHY_FLAG_PHY_UP	(1u << 0)  /**< Flag to track physical state */
#define XVPHY_FLAG_IS_REPEATER	(1u << 1)  /**< Flag to track repeater state */

#define XVPHY_TMO_5MS		(   5u)    /**< Timeout value for 5ms */
#define XVPHY_TMO_100MS		( 100u)    /**< Timeout value for 100ms */
#define XVPHY_TMO_1SECOND	(1000u)    /**< Timeout value for 1s */

/**************************** Type Definitions *******************************/

typedef enum {
	XHDCP1X_EVENT_NULL,
	XHDCP1X_EVENT_AUTHENTICATE,
	XHDCP1X_EVENT_CHECK,
	XHDCP1X_EVENT_DISABLE,
	XHDCP1X_EVENT_ENABLE,
	XHDCP1X_EVENT_LINKDOWN,
	XHDCP1X_EVENT_PHYDOWN,
	XHDCP1X_EVENT_PHYUP,
	XHDCP1X_EVENT_POLL,
	XHDCP1X_EVENT_TIMEOUT,
} XHdcp1x_EventType;

typedef enum {
	XHDCP1X_STATE_DISABLED,
	XHDCP1X_STATE_DETERMINERXCAPABLE,
	XHDCP1X_STATE_EXCHANGEKSVS,
	XHDCP1X_STATE_COMPUTATIONS,
	XHDCP1X_STATE_VALIDATERX,
	XHDCP1X_STATE_AUTHENTICATED,
	XHDCP1X_STATE_LINKINTEGRITYCHECK,
	XHDCP1X_STATE_TESTFORREPEATER,
	XHDCP1X_STATE_WAITFORREADY,
	XHDCP1X_STATE_READKSVLIST,
	XHDCP1X_STATE_UNAUTHENTICATED,
	XHDCP1X_STATE_PHYDOWN,
} XHdcp1x_StateType;

/***************** Macros (Inline Functions) Definitions *********************/

/*************************** Function Prototypes *****************************/

static void XHdcp1x_TxDebugLog(const XHdcp1x *InstancePtr, const char *LogMsg);
static void XHdcp1x_TxPostEvent(XHdcp1x *InstancePtr, XHdcp1x_EventType Event);
static void XHdcp1x_TxStartTimer(XHdcp1x *InstancePtr, u16 TimeoutInMs);
static void XHdcp1x_TxStopTimer(XHdcp1x *InstancePtr);
static void XHdcp1x_TxBusyDelay(XHdcp1x *InstancePtr, u16 DelayInMs);
static void XHdcp1x_TxReauthenticateCallback(void *Parameter);
static void XHdcp1x_TxCheckLinkCallback(void *Parameter);
static void XHdcp1x_TxSetCheckLinkState(XHdcp1x *InstancePtr, int IsEnabled);
static void XHdcp1x_TxEnableEncryptionState(XHdcp1x *InstancePtr);
static void XHdcp1x_TxDisableEncryptionState(XHdcp1x *InstancePtr);
static void XHdcp1x_TxEnableState(XHdcp1x *InstancePtr);
static void XHdcp1x_TxDisableState(XHdcp1x *InstancePtr);
static void XHdcp1x_TxCheckRxCapable(const XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static u64 XHdcp1x_TxGenerateAn(XHdcp1x *InstancePtr);
static int XHdcp1x_TxIsKsvValid(u64 Ksv);
static void XHdcp1x_TxExchangeKsvs(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxStartComputations(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxPollForComputations(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxValidateRx(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxCheckLinkIntegrity(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxTestForRepeater(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxPollForWaitForReady(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static int XHdcp1x_TxValidateKsvList(XHdcp1x *InstancePtr, u16 RepeaterInfo);
static void XHdcp1x_TxReadKsvList(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunDisabledState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunDetermineRxCapableState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunExchangeKsvsState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunComputationsState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunValidateRxState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunAuthenticatedState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunLinkIntegrityCheckState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunTestForRepeaterState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunWaitForReadyState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunReadKsvListState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunUnauthenticatedState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxRunPhysicalLayerDownState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxEnterState(XHdcp1x *InstancePtr, XHdcp1x_StateType State,
		XHdcp1x_StateType *NextStatePtr);
static void XHdcp1x_TxExitState(XHdcp1x *InstancePtr, XHdcp1x_StateType State);
static void XHdcp1x_TxDoTheState(XHdcp1x *InstancePtr, XHdcp1x_EventType Event);
static void XHdcp1x_TxProcessPending(XHdcp1x *InstancePtr);
static const char *XHdcp1x_TxStateToString(XHdcp1x_StateType State);

/************************** Function Definitions *****************************/

/*****************************************************************************/
/**
* This function initializes a transmit state machine.
*
* @param	InstancePtr is the receiver instance.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void XHdcp1x_TxInit(XHdcp1x *InstancePtr)
{
	XHdcp1x_StateType DummyState = XHDCP1X_STATE_DISABLED;

	/* Update theHandler */
	InstancePtr->Tx.PendingEvents = 0;

	/* Kick the state machine */
	XHdcp1x_TxEnterState(InstancePtr, XHDCP1X_STATE_DISABLED, &DummyState);
}

/*****************************************************************************/
/**
* This function polls an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxPoll(XHdcp1x *InstancePtr)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Process any pending events */
	XHdcp1x_TxProcessPending(InstancePtr);

	/* Poll it */
	XHdcp1x_TxDoTheState(InstancePtr, XHDCP1X_EVENT_POLL);

	return (Status);
}

/*****************************************************************************/
/**
* This function resets an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		This function disables and then re-enables the interface.
*
******************************************************************************/
int XHdcp1x_TxReset(XHdcp1x *InstancePtr)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Reset it */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_DISABLE);
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_ENABLE);

	return (Status);
}

/*****************************************************************************/
/**
* This function enables an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxEnable(XHdcp1x *InstancePtr)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Post it */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_ENABLE);

	return (Status);
}

/*****************************************************************************/
/**
* This function disables an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxDisable(XHdcp1x *InstancePtr)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Post it */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_DISABLE);

	return (Status);
}

/*****************************************************************************/
/**
* This function updates the physical state of an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
* @param	IsUp is truth value indicating the status of physical interface.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxSetPhysicalState(XHdcp1x *InstancePtr, int IsUp)
{
	int Status = XST_SUCCESS;
	XHdcp1x_EventType Event = XHDCP1X_EVENT_PHYDOWN;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Determine Event */
	if (IsUp) {
		Event = XHDCP1X_EVENT_PHYUP;
	}

	/* Post it */
	XHdcp1x_TxPostEvent(InstancePtr, Event);

	return (Status);
}

/*****************************************************************************/
/**
* This function set the lane count of an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
* @param	LaneCount is the number of lanes of the interface.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxSetLaneCount(XHdcp1x *InstancePtr, int LaneCount)
{
	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);
	Xil_AssertNonvoid(LaneCount > 0);

	/* Set it */
	return (XHdcp1x_CipherSetNumLanes(InstancePtr, LaneCount));
}

/*****************************************************************************/
/**
* This function initiates authentication on an interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxAuthenticate(XHdcp1x *InstancePtr)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Post the re-authentication request */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_AUTHENTICATE);

	return (Status);
}

/*****************************************************************************/
/**
* This function queries an interface to check if authentication is still in
* progress.
*
* @param	InstancePtr is the transmitter instance.
*
* @return	Truth value indicating in progress (true) or not (false).
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxIsInProgress(const XHdcp1x *InstancePtr)
{
	int IsInProgress = FALSE;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Which state? */
	switch (InstancePtr->Tx.CurrentState) {
		/* For the "steady" states */
		case XHDCP1X_STATE_DISABLED:
		case XHDCP1X_STATE_UNAUTHENTICATED:
		case XHDCP1X_STATE_AUTHENTICATED:
		case XHDCP1X_STATE_LINKINTEGRITYCHECK:
			IsInProgress = FALSE;
			break;

		/* Otherwise */
		default:
			IsInProgress = TRUE;
			break;
	}

	return (IsInProgress);
}

/*****************************************************************************/
/**
* This function queries an interface to check if its been authenticated.
*
* @param	InstancePtr is the transmitter instance.
*
* @return	Truth value indicating authenticated (true) or not (false).
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxIsAuthenticated(const XHdcp1x *InstancePtr)
{
	int Authenticated = FALSE;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Which state? */
	switch (InstancePtr->Tx.CurrentState) {
		/* For the authenticated and link integrity check states */
		case XHDCP1X_STATE_AUTHENTICATED:
		case XHDCP1X_STATE_LINKINTEGRITYCHECK:
			Authenticated = TRUE;
			break;

		/* Otherwise */
		default:
			Authenticated = FALSE;
			break;
	}

	return (Authenticated);
}

/*****************************************************************************/
/**
* This function retrieves the current encryption stream map.
*
* @param	InstancePtr the transmitter instance.
*
* @return	The current encryption stream map.
*
* @note		None.
*
******************************************************************************/
u64 XHdcp1x_TxGetEncryption(const XHdcp1x *InstancePtr)
{
	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	return (InstancePtr->Tx.EncryptionMap);
}

/*****************************************************************************/
/**
* This function enables encryption on set of streams on an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
* @param	StreamMap is the bit map of streams to enable encryption on.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxEnableEncryption(XHdcp1x *InstancePtr, u64 StreamMap)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Update InstancePtr */
	InstancePtr->Tx.EncryptionMap |= StreamMap;

	/* Check for authenticated */
	if (XHdcp1x_TxIsAuthenticated(InstancePtr)) {
		XHdcp1x_TxEnableEncryptionState(InstancePtr);
	}

	return (Status);
}

/*****************************************************************************/
/**
* This function disables encryption on set of streams on an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
* @param	StreamMap is the bit map of streams to disable encryption on.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxDisableEncryption(XHdcp1x *InstancePtr, u64 StreamMap)
{
	int Status = XST_SUCCESS;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Disable it */
	Status = XHdcp1x_CipherDisableEncryption(InstancePtr, StreamMap);

	/* Update InstancePtr */
	if (Status == XST_SUCCESS) {
		InstancePtr->Tx.EncryptionMap &= ~StreamMap;
	}

	return (Status);
}

/*****************************************************************************/
/**
* This function handles a timeout on an HDCP interface.
*
* @param	InstancePtr is the transmitter instance.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void XHdcp1x_TxHandleTimeout(XHdcp1x *InstancePtr)
{
	/* Verify arguments. */
	Xil_AssertVoid(InstancePtr != NULL);

	/* Post the timeout */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_TIMEOUT);
}

/*****************************************************************************/
/**
* This function implements the debug display output for transmit instances.
*
* @param	InstancePtr is the receiver instance.
*
* @return
*		- XST_SUCCESS if successful.
*
* @note		None.
*
******************************************************************************/
int XHdcp1x_TxInfo(const XHdcp1x *InstancePtr)
{
	u32 Version = 0;

	/* Verify arguments. */
	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Display it */
	XHDCP1X_DEBUG_PRINTF("Type:            ");
	if (InstancePtr->Config.IsHDMI) {
		XHDCP1X_DEBUG_PRINTF("hdmi-tx\r\n");
	}
	else {
		XHDCP1X_DEBUG_PRINTF("dp-tx\r\n");
	}
	XHDCP1X_DEBUG_PRINTF("Current State:   %s\r\n",
			XHdcp1x_TxStateToString(InstancePtr->Tx.CurrentState));
	XHDCP1X_DEBUG_PRINTF("Previous State:  %s\r\n",
			XHdcp1x_TxStateToString(InstancePtr->Tx.PreviousState));
	XHDCP1X_DEBUG_PRINTF("State Helper:    %016llX\r\n",
			InstancePtr->Tx.StateHelper);
	XHDCP1X_DEBUG_PRINTF("Flags:           %04X\r\n",
			InstancePtr->Tx.Flags);
	XHDCP1X_DEBUG_PRINTF("Encryption Map:  %016llX\r\n",
			InstancePtr->Tx.EncryptionMap);
	Version = XHdcp1x_GetDriverVersion();
	XHDCP1X_DEBUG_PRINTF("Driver Version:  %d.%02d.%02d\r\n",
			((Version >> 16) &0xFFFFu), ((Version >> 8) & 0xFFu),
			(Version & 0xFFu));
	Version = XHdcp1x_CipherGetVersion(InstancePtr);
	XHDCP1X_DEBUG_PRINTF("Cipher Version:  %d.%02d.%02d\r\n",
			((Version >> 16) &0xFFFFu), ((Version >> 8) & 0xFFu),
			(Version & 0xFFu));
	XHDCP1X_DEBUG_PRINTF("\r\n");
	XHDCP1X_DEBUG_PRINTF("Tx Stats\r\n");
	XHDCP1X_DEBUG_PRINTF("Auth Passed:     %d\r\n",
			InstancePtr->Tx.Stats.AuthPassed);
	XHDCP1X_DEBUG_PRINTF("Auth Failed:     %d\r\n",
			InstancePtr->Tx.Stats.AuthFailed);
	XHDCP1X_DEBUG_PRINTF("Reauth Requests: %d\r\n",
			InstancePtr->Tx.Stats.ReauthRequested);
	XHDCP1X_DEBUG_PRINTF("Check Passed:    %d\r\n",
			InstancePtr->Tx.Stats.LinkCheckPassed);
	XHDCP1X_DEBUG_PRINTF("Check Failed:    %d\r\n",
			InstancePtr->Tx.Stats.LinkCheckFailed);
	XHDCP1X_DEBUG_PRINTF("Read Failures:   %d\r\n",
			InstancePtr->Tx.Stats.ReadFailures);

	XHDCP1X_DEBUG_PRINTF("\r\n");
	XHDCP1X_DEBUG_PRINTF("Cipher Stats\r\n");
	XHDCP1X_DEBUG_PRINTF("Int Count:       %d\r\n",
			InstancePtr->Cipher.Stats.IntCount);

	XHDCP1X_DEBUG_PRINTF("\r\n");
	XHDCP1X_DEBUG_PRINTF("Port Stats\r\n");
	XHDCP1X_DEBUG_PRINTF("Int Count:       %d\r\n",
			InstancePtr->Port.Stats.IntCount);

	return (XST_SUCCESS);
}

/*****************************************************************************/
/**
* This function logs a debug message on behalf of a handler state machine.
*
* @param	InstancePtr is the receiver instance.
* @param	LogMsg is the message to log.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxDebugLog(const XHdcp1x *InstancePtr, const char *LogMsg)
{
	char Label[16];

	/* Format Label */
	snprintf(Label, 16, "hdcp-tx(%d) - ", InstancePtr->Config.DeviceId);

	/* Log it */
	XHDCP1X_DEBUG_LOGMSG(Label);
	XHDCP1X_DEBUG_LOGMSG(LogMsg);
	XHDCP1X_DEBUG_LOGMSG("\r\n");
}

/*****************************************************************************/
/**
* This function posts an event to a state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to post.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxPostEvent(XHdcp1x *InstancePtr, XHdcp1x_EventType Event)
{
	/* Check for disable and clear any pending enable */
	if (Event == XHDCP1X_EVENT_DISABLE) {
		InstancePtr->Tx.PendingEvents &= ~(1u << XHDCP1X_EVENT_ENABLE);
	}
	/* Check for phy-down and clear any pending phy-up */
	else if (Event == XHDCP1X_EVENT_PHYDOWN) {
		InstancePtr->Tx.PendingEvents &= ~(1u << XHDCP1X_EVENT_PHYUP);
	}

	/* Post it */
	InstancePtr->Tx.PendingEvents |= (1u << Event);
}

/*****************************************************************************/
/**
* This function starts a state machine's timer.
*
* @param	InstancePtr is the state machine.
* @param	TimeoutInMs is the timeout in milli-seconds.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxStartTimer(XHdcp1x *InstancePtr, u16 TimeoutInMs)
{
	/* Start it */
	XHdcp1x_PlatformTimerStart(InstancePtr, TimeoutInMs);
}

/*****************************************************************************/
/**
* This function stops a state machine's timer.
*
* @param	InstancePtr is the state machine.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxStopTimer(XHdcp1x *InstancePtr)
{
	/* Stop it */
	XHdcp1x_PlatformTimerStop(InstancePtr);
}

/*****************************************************************************/
/**
*
* This function busy delays a state machine.
*
* @param	InstancePtr is the state machine.
* @param	TimeoutInMs is the delay time in milli-seconds.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxBusyDelay(XHdcp1x *InstancePtr, u16 DelayInMs)
{
	/* Busy wait */
	XHdcp1x_PlatformTimerBusy(InstancePtr, DelayInMs);
}

/*****************************************************************************/
/**
* This function acts as the reauthentication callback for a state machine.
*
* @param	Parameter is the parameter specified during registration.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxReauthenticateCallback(void *Parameter)
{
	XHdcp1x *InstancePtr = Parameter;

	/* Update statistics */
	InstancePtr->Tx.Stats.ReauthRequested++;

	/* Post the re-authentication request */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_AUTHENTICATE);
}

/*****************************************************************************/
/**
* This function acts as the check link callback for a state machine.
*
* @param	Parameter is the parameter specified during registration.
*
* @return	Mpme/
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxCheckLinkCallback(void *Parameter)
{
	XHdcp1x *InstancePtr = Parameter;

	/* Post the check request */
	XHdcp1x_TxPostEvent(InstancePtr, XHDCP1X_EVENT_CHECK);
}

/*****************************************************************************/
/**
* This function sets the check link state of the handler.
*
* @param	InstancePtr is the HDCP state machine.
* @param	IsEnabled is truth value indicating on/off.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxSetCheckLinkState(XHdcp1x *InstancePtr, int IsEnabled)
{
	/* Check for HDMI */
	if (InstancePtr->Config.IsHDMI) {
		/* Check for enabled */
		if (IsEnabled) {
			/* Register Callback */
			XHdcp1x_CipherSetCallback(InstancePtr,
				XHDCP1X_CIPHER_HANDLER_Ri_UPDATE,
				&XHdcp1x_TxCheckLinkCallback, InstancePtr);

			/* Enable it */
			XHdcp1x_CipherSetRiUpdate(InstancePtr, TRUE);
		}
		/* Otherwise */
		else {
			/* Disable it */
			XHdcp1x_CipherSetRiUpdate(InstancePtr, FALSE);
		}
	}
}

/*****************************************************************************/
/**
* This function enables encryption for a state machine.
*
* @param	InstancePtr is the HDCP state machine.
*
* @return	None.
*
* @note		This function inserts a 5ms delay for things to settle when
*		encryption is actually being disabled.
*
******************************************************************************/
static void XHdcp1x_TxEnableEncryptionState(XHdcp1x *InstancePtr)
{
	/* Check for encryption enabled */
	if (InstancePtr->Tx.EncryptionMap != 0) {
		u64 StreamMap = 0;

		/* Determine StreamMap */
		StreamMap =
			XHdcp1x_CipherGetEncryption(InstancePtr);

		/* Check if there is something to do */
		if (StreamMap != InstancePtr->Tx.EncryptionMap) {
			/* Wait a bit */
			XHdcp1x_TxBusyDelay(InstancePtr, XVPHY_TMO_5MS);

			/* Enable it */
			XHdcp1x_CipherEnableEncryption(InstancePtr,
					InstancePtr->Tx.EncryptionMap);
		}
	}
}

/*****************************************************************************/
/**
* This function disables encryption for a state machine.
*
* @param	InstancePtr is the HDCP state machine.
*
* @return	None.
*
* @note		This function inserts a 5ms delay for things to settle when
*		encryption is actually being disabled.
*
******************************************************************************/
static void XHdcp1x_TxDisableEncryptionState(XHdcp1x *InstancePtr)
{
	u64 StreamMap = XHdcp1x_CipherGetEncryption(InstancePtr);

	/* Check if encryption actually enabled */
	if (StreamMap != 0) {
		/* Update StreamMap for all stream */
		StreamMap = (u64)(-1);

		/* Disable it all */
		XHdcp1x_CipherDisableEncryption(InstancePtr, StreamMap);

		/* Wait at least a frame */
		XHdcp1x_TxBusyDelay(InstancePtr, XVPHY_TMO_5MS);
	}
}

/*****************************************************************************/
/**
* This function enables a state machine.
*
* @param	InstancePtr is the HDCP state machine.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxEnableState(XHdcp1x *InstancePtr)
{
	/* Clear statistics */
	memset(&(InstancePtr->Tx.Stats), 0, sizeof(InstancePtr->Tx.Stats));

	/* Enable the crypto engine */
	XHdcp1x_CipherEnable(InstancePtr);

	/* Register the re-authentication callback */
	XHdcp1x_PortSetCallback(InstancePtr,
			XHDCP1X_PORT_HANDLER_AUTHENTICATE,
			&XHdcp1x_TxReauthenticateCallback, InstancePtr);

	/* Enable the hdcp port */
	XHdcp1x_PortEnable(InstancePtr);
}

/*****************************************************************************/
/**
* This function disables a state machine.
*
* @param	InstancePtr is the HDCP state machine.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxDisableState(XHdcp1x *InstancePtr)
{
	/* Disable the hdcp port */
	XHdcp1x_PortDisable(InstancePtr);

	/* Disable the cryto engine */
	XHdcp1x_CipherDisable(InstancePtr);

	/* Disable the timer */
	XHdcp1x_TxStopTimer(InstancePtr);

	/* Update InstancePtr */
	InstancePtr->Tx.Flags &= ~XVPHY_FLAG_IS_REPEATER;
	InstancePtr->Tx.StateHelper = 0;
	InstancePtr->Tx.EncryptionMap = 0;
}

/*****************************************************************************/
/**
* This function checks to ensure that the remote end is HDCP capable.
*
* @param	InstancePtr is the HDCP state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxCheckRxCapable(const XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	/* Check for capable */
	if (XHdcp1x_PortIsCapable(InstancePtr)) {
		/* Log */
		XHdcp1x_TxDebugLog(InstancePtr, "rx hdcp capable");

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_EXCHANGEKSVS;
	}
	else {
		/* Log */
		XHdcp1x_TxDebugLog(InstancePtr, "rx not capable");

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;
	}
}

/*****************************************************************************/
/**
* This function generates the An from a random number generator.
*
* @param	InstancePtr is the HDCP state machine.
*
* @return	A 64-bit pseudo random number (An).
*
* @note		None.
*
******************************************************************************/
static u64 XHdcp1x_TxGenerateAn(XHdcp1x *InstancePtr)
{
	u64 An = 0;

	/* Attempt to generate An */
	if (XHdcp1x_CipherDoRequest(InstancePtr, XHDCP1X_CIPHER_REQUEST_RNG) ==
			XST_SUCCESS) {
		/* Wait until done */
		while (!XHdcp1x_CipherIsRequestComplete(InstancePtr));

		/* Update theAn */
		An = XHdcp1x_CipherGetMi(InstancePtr);
	}

	/* Check if zero */
	if (An == 0) {
		An = 0x351F7175406A74Dull;
	}

	return (An);
}

/*****************************************************************************/
/**
* This function validates a KSV value as having 20 1s and 20 0s.
*
* @param	Ksv is the value to validate.
*
* @return	Truth value indicating valid (TRUE) or not (FALSE).
*
* @note		None.
*
******************************************************************************/
static int XHdcp1x_TxIsKsvValid(u64 Ksv)
{
	int IsValid = FALSE;
	int NumOnes = 0;

	/* Determine NumOnes */
	while (Ksv != 0) {
		if ((Ksv & 1) != 0) {
			NumOnes++;
		}
		Ksv >>= 1;
	}

	/* Check for 20 1s */
	if (NumOnes == 20) {
		IsValid = TRUE;
	}

	return (IsValid);
}

/*****************************************************************************/
/**
* This function exchanges the ksvs between the two ends of the link.
*
* @param	InstancePtr is the HDCP state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxExchangeKsvs(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	u8 Buf[8];

	/* Initialize Buf */
	memset(Buf, 0, 8);

	/* Update NextStatePtr - assume failure */
	*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;

	/* Read the Bksv from remote end */
	if (XHdcp1x_PortRead(InstancePtr, XHDCP1X_PORT_OFFSET_BKSV,
			Buf, 5) > 0) {
		u64 RemoteKsv = 0;

		/* Determine theRemoteKsv */
		XHDCP1X_PORT_BUF_TO_UINT(RemoteKsv, Buf,
				XHDCP1X_PORT_SIZE_BKSV * 8);

		/* Check for invalid */
		if (!XHdcp1x_TxIsKsvValid(RemoteKsv)) {
			XHdcp1x_TxDebugLog(InstancePtr, "Bksv invalid");
		}
		/* Check for revoked */
		else if (XHdcp1x_PlatformIsKsvRevoked(InstancePtr, RemoteKsv)) {
			XHdcp1x_TxDebugLog(InstancePtr, "Bksv is revoked");
		}
		/* Otherwise we're good to go */
		else {
			u64 LocalKsv = 0;
			u64 An = 0;

			/* Check for repeater and update InstancePtr */
			if (XHdcp1x_PortIsRepeater(InstancePtr)) {
				InstancePtr->Tx.Flags |= XVPHY_FLAG_IS_REPEATER;
			}
			else {
				InstancePtr->Tx.Flags &=
							~XVPHY_FLAG_IS_REPEATER;
			}

			/* Generate theAn */
			An = XHdcp1x_TxGenerateAn(InstancePtr);

			/* Save theAn into the state helper for use later */
			InstancePtr->Tx.StateHelper = An;

			/* Determine theLocalKsv */
			LocalKsv = XHdcp1x_CipherGetLocalKsv(InstancePtr);

			/* Load the cipher with the remote ksv */
			XHdcp1x_CipherSetRemoteKsv(InstancePtr, RemoteKsv);

			/* Send An to remote */
			XHDCP1X_PORT_UINT_TO_BUF(Buf, An,
					XHDCP1X_PORT_SIZE_AN * 8);
			XHdcp1x_PortWrite(InstancePtr, XHDCP1X_PORT_OFFSET_AN,
					Buf, XHDCP1X_PORT_SIZE_AN);

			/* Send AKsv to remote */
			XHDCP1X_PORT_UINT_TO_BUF(Buf, LocalKsv,
					XHDCP1X_PORT_SIZE_AKSV * 8);
			XHdcp1x_PortWrite(InstancePtr, XHDCP1X_PORT_OFFSET_AKSV,
					Buf, XHDCP1X_PORT_SIZE_AKSV);

			/* Update NextStatePtr */
			*NextStatePtr = XHDCP1X_STATE_COMPUTATIONS;
		}
	}
	/* Otherwise */
	else {
		/* Update the statistics */
		InstancePtr->Tx.Stats.ReadFailures++;
	}
}

/*****************************************************************************/
/**
* This function initiates the computations for a state machine.
*
* @param	InstancePtr is the HDCP receiver state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxStartComputations(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	u64 Value = 0;
	u32 X = 0;
	u32 Y = 0;
	u32 Z = 0;

	/* Log */
	XHdcp1x_TxDebugLog(InstancePtr, "starting computations");

	/* Update Value with An */
	Value = InstancePtr->Tx.StateHelper;

	/* Load the cipher B registers with An */
	X = (u32) (Value & 0x0FFFFFFFul);
	Value >>= 28;
	Y = (u32) (Value & 0x0FFFFFFFul);
	Value >>= 28;
	Z = (u32) (Value & 0x000000FFul);
	if ((InstancePtr->Tx.Flags & XVPHY_FLAG_IS_REPEATER) != 0) {
		Z |= (1ul << 8);
	}
	XHdcp1x_CipherSetB(InstancePtr, X, Y, Z);

	/* Initiate the block cipher */
	XHdcp1x_CipherDoRequest(InstancePtr, XHDCP1X_CIPHER_REQUEST_BLOCK);
}

/*****************************************************************************/
/**
* This function polls the progress of the computations for a state machine.
*
* @param	InstancePtr is the HDCP state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxPollForComputations(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	/* Check for done */
	if (XHdcp1x_CipherIsRequestComplete(InstancePtr)) {
		XHdcp1x_TxDebugLog(InstancePtr, "computations complete");
		*NextStatePtr = XHDCP1X_STATE_VALIDATERX;
	}
	else {
		XHdcp1x_TxDebugLog(InstancePtr, "waiting for computations");
	}
}

/*****************************************************************************/
/**
* This function validates the attached receiver.
*
* @param	InstancePtr is the HDCP state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxValidateRx(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	u8 Buf[2];
	int NumTries = 3;

	/* Update NextStatePtr */
	*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;

	/* Attempt to read Ro */
	do {
		/* Read the remote Ro' */
		if (XHdcp1x_PortRead(InstancePtr, XHDCP1X_PORT_OFFSET_RO,
				Buf, 2) > 0) {
			char LogBuf[32];
			u16 RemoteRo = 0;
			u16 LocalRo = 0;

			/* Determine RemoteRo */
			XHDCP1X_PORT_BUF_TO_UINT(RemoteRo, Buf, 2 * 8);

			/* Determine theLLocalRoocalRo */
			LocalRo = XHdcp1x_CipherGetRo(InstancePtr);

			/* Compare the Ro values */
			if (LocalRo == RemoteRo) {

				/* Determine theLogBuf */
				snprintf(LogBuf, 32, "rx valid Ro/Ro' (%04X)",
						LocalRo);

				/* Update NextStatePtr */
				*NextStatePtr = XHDCP1X_STATE_TESTFORREPEATER;
			}
			/* Otherwise */
			else {
				/* Determine theLogBuf */
				snprintf(LogBuf, 32, "Ro/Ro' mismatch (%04X/"
						"%04X)", LocalRo, RemoteRo);

				/* Update statistics if the last attempt */
				if (NumTries == 1)
					InstancePtr->Tx.Stats.AuthFailed++;
			}

			/* Log */
			XHdcp1x_TxDebugLog(InstancePtr, LogBuf);
		}
		/* Otherwise */
		else {
			/* Log */
			XHdcp1x_TxDebugLog(InstancePtr, "Ro' read failure");

			/* Update the statistics */
			InstancePtr->Tx.Stats.ReadFailures++;
		}

		/* Update for loop */
		NumTries--;
	}
	while ((*NextStatePtr == XHDCP1X_STATE_UNAUTHENTICATED) &&
		(NumTries > 0));
}

/*****************************************************************************/
/**
* This function checks the integrity of a HDCP link.
*
* @param	InstancePtr is the hdcp state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxCheckLinkIntegrity(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	u8 Buf[2];
	int NumTries = 3;

	/* Update theNextState */
	*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;

	/* Iterate through the tries */
	do {
		/* Read the remote Ri' */
		if (XHdcp1x_PortRead(InstancePtr, XHDCP1X_PORT_OFFSET_RO,
				Buf, 2) > 0) {

			char LogBuf[48];
			u16 RemoteRi = 0;
			u16 LocalRi = 0;

			/* Determine theRemoteRo */
			XHDCP1X_PORT_BUF_TO_UINT(RemoteRi, Buf, 16);

			/* Determine theLocalRi */
			LocalRi = XHdcp1x_CipherGetRi(InstancePtr);

			/* Compare the local and remote values */
			if (LocalRi == RemoteRi) {
				*NextStatePtr = XHDCP1X_STATE_AUTHENTICATED;
				snprintf(LogBuf, 48, "link check passed Ri/Ri'"
						"(%04X)", LocalRi);
			}
			/* Check for last attempt */
			else if (NumTries == 1) {
				snprintf(LogBuf, 48, "link check failed Ri/Ri'"
						"(%04X/%04X)", LocalRi,
						RemoteRi);
			}

			/* Log */
			XHdcp1x_TxDebugLog(InstancePtr, LogBuf);
		}
		else {
			XHdcp1x_TxDebugLog(InstancePtr, "Ri' read failure");
			InstancePtr->Tx.Stats.ReadFailures++;
		}

		/* Update for loop */
		NumTries--;
	} while ((*NextStatePtr != XHDCP1X_STATE_AUTHENTICATED) &&
		(NumTries > 0));

	/* Check for success */
	if (*NextStatePtr == XHDCP1X_STATE_AUTHENTICATED) {
		InstancePtr->Tx.Stats.LinkCheckPassed++;
	}
	else {
		InstancePtr->Tx.Stats.LinkCheckFailed++;
	}
}

/*****************************************************************************/
/**
* This function checks the remote end to see if its a repeater.
*
* @param	InstancePtr is the HDCP state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		The implementation of this function enables encryption when a
*		repeater is detected downstream. The standard is ambiguous as to
*		the handling of this specific case by this behaviour is required
*		in order to pass the Unigraf compliance test suite.
*
******************************************************************************/
static void XHdcp1x_TxTestForRepeater(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	/* Check for repeater */
	if (XHdcp1x_PortIsRepeater(InstancePtr)) {
		u8 Buf[XHDCP1X_PORT_SIZE_AINFO];

		/* Update InstancePtr */
		InstancePtr->Tx.Flags |= XVPHY_FLAG_IS_REPEATER;

		/* Clear AINFO */
		memset(Buf, 0, XHDCP1X_PORT_SIZE_AINFO);
		XHdcp1x_PortWrite(InstancePtr, XHDCP1X_PORT_OFFSET_AINFO, Buf,
				XHDCP1X_PORT_SIZE_AINFO);

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_WAITFORREADY;

		/* Log */
		XHdcp1x_TxDebugLog(InstancePtr, "repeater detected");

		/* Enable authentication if needed */
		XHdcp1x_TxEnableEncryptionState(InstancePtr);
	}
	else {
		/* Update InstancePtr */
		InstancePtr->Tx.Flags &= ~XVPHY_FLAG_IS_REPEATER;

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_AUTHENTICATED;
	}
}

/*****************************************************************************/
/**
* This function polls a state machine in the "wait for ready" state.
*
* @param	InstancePtr is the hdcp state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxPollForWaitForReady(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	u16 RepeaterInfo = 0;
	int Status = XST_SUCCESS;

	/* Attempt to read the repeater info */
	Status = XHdcp1x_PortGetRepeaterInfo(InstancePtr, &RepeaterInfo);
	if (Status == XST_SUCCESS) {
		/* Check that neither cascade or device numbers exceeded */
		if ((RepeaterInfo & 0x0880u) == 0) {
			/* Check for at least one attached device */
			if ((RepeaterInfo & 0x007Fu) != 0) {
				/* Update InstancePtr */
				InstancePtr->Tx.StateHelper = RepeaterInfo;

				/* Update NextStatePtr */
				*NextStatePtr = XHDCP1X_STATE_READKSVLIST;

				/* Log */
				XHdcp1x_TxDebugLog(InstancePtr,
					"devices attached: ksv list ready");
			}
			/* Otherwise */
			else {
				/* Update NextStatePtr */
				*NextStatePtr = XHDCP1X_STATE_AUTHENTICATED;

				/* Log */
				XHdcp1x_TxDebugLog(InstancePtr,
					"no attached devices");
			}
		}
		/* Check for cascade exceeded */
		else {
			/* Update NextStatePtr */
			*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;

			/* Log */
			if ((RepeaterInfo & 0x0800u) != 0) {
				XHdcp1x_TxDebugLog(InstancePtr,
					"max cascade exceeded");
			}
			else {
				XHdcp1x_TxDebugLog(InstancePtr,
					"max devices exceeded");
			}
		}
	}
}

/*****************************************************************************/
/**
* This function validates the ksv list from an attached repeater.
*
* @param	InstancePtr is the hdcp state machine.
* @param	RepeaterInfo is the repeater information.
*
* @return	Truth value indicating valid (TRUE) or invalid (FALSE).
*
* @note		None.
*
******************************************************************************/
static int XHdcp1x_TxValidateKsvList(XHdcp1x *InstancePtr, u16 RepeaterInfo)
{
	SHA1Context Sha1Context;
	u8 Buf[24];
	int NumToRead = 0;
	int IsValid = FALSE;

	/* Initialize Buf */
	memset(Buf, 0, 24);

	/* Initialize Sha1Context */
	SHA1Reset(&Sha1Context);

	/* Assume success */
	IsValid = TRUE;

	/* Determine theNumToRead */
	NumToRead = ((RepeaterInfo & 0x7Fu)*5);

	/* Read the ksv list */
	do {
		int NumThisTime = XHDCP1X_PORT_SIZE_KSVFIFO;

		/* Truncate if necessary */
		if (NumThisTime > NumToRead) {
			NumThisTime = NumToRead;
		}

		/* Read the next chunk of the list */
		if (XHdcp1x_PortRead(InstancePtr, XHDCP1X_PORT_OFFSET_KSVFIFO,
				Buf, NumThisTime) > 0) {
			/* Update the calculation of V */
			SHA1Input(&Sha1Context, Buf, NumThisTime);
		}
		else {
			/* Update the statistics */
			InstancePtr->Tx.Stats.ReadFailures++;

			/* Update IsValid */
			IsValid = FALSE;
		}

		/* Update for loop */
		NumToRead -= NumThisTime;
	}
	while ((NumToRead > 0) && (IsValid));

	/* Check for success */
	if (IsValid) {
		u64 Mo = 0;
		u8 Sha1Result[SHA1HashSize];

		/* Insert RepeaterInfo into the SHA-1 transform */
		Buf[0] = (u8) (RepeaterInfo & 0xFFu);
		Buf[1] = (u8) ((RepeaterInfo >> 8) & 0xFFu);
		SHA1Input(&Sha1Context, Buf, 2);

		/* Insert the Mo into the SHA-1 transform */
		Mo = XHdcp1x_CipherGetMo(InstancePtr);
		XHDCP1X_PORT_UINT_TO_BUF(Buf, Mo, 64);
		SHA1Input(&Sha1Context, Buf, 8);

		/* Finalize the SHA-1 result and confirm success */
		if (SHA1Result(&Sha1Context, Sha1Result) == shaSuccess) {
			u8 Offset = XHDCP1X_PORT_OFFSET_VH0;
			const u8 *Sha1Buf = Sha1Result;
			int NumIterations = (SHA1HashSize >> 2);

			/* Iterate through the SHA-1 chunks */
			do {
				u32 CalcValue = 0;
				u32 ReadValue = 0;

				/* Determine CalcValue */
				CalcValue = *Sha1Buf++;
				CalcValue <<= 8;
				CalcValue |= *Sha1Buf++;
				CalcValue <<= 8;
				CalcValue |= *Sha1Buf++;
				CalcValue <<= 8;
				CalcValue |= *Sha1Buf++;

				/* Read the value from the far end */
				if (XHdcp1x_PortRead(InstancePtr, Offset, Buf,
						4) > 0) {
					/* Determine ReadValue */
					XHDCP1X_PORT_BUF_TO_UINT(ReadValue,
							Buf, 32);
				}
				else {
					/* Update ReadValue */
					ReadValue = 0;

					/* Update the statistics */
					InstancePtr->Tx.Stats.ReadFailures++;
				}

				/* Check for mismatch */
				if (CalcValue != ReadValue) {
					IsValid = FALSE;
				}

				/* Update for loop */
				Offset += 4;
				NumIterations--;
			}
			while (NumIterations > 0);
		}
		/* Otherwise */
		else {
			IsValid = FALSE;
		}
	}

	return (IsValid);
}

/*****************************************************************************/
/**
* This function reads the ksv list from an attached repeater.
*
* @param	InstancePtr is the hdcp state machine.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxReadKsvList(XHdcp1x *InstancePtr,
		XHdcp1x_StateType *NextStatePtr)
{
	int NumAttempts = 3;
	int KsvListIsValid = FALSE;
	u16 RepeaterInfo = 0;

	/* Determine RepeaterInfo */
	RepeaterInfo = (u16)(InstancePtr->Tx.StateHelper & 0x0FFFu);

	/* Iterate through the attempts */
	do {
		/* Attempt to validate the ksv list */
		KsvListIsValid =
			XHdcp1x_TxValidateKsvList(InstancePtr, RepeaterInfo);

		/* Update for loop */
		NumAttempts--;
	}
	while ((NumAttempts > 0) && (!KsvListIsValid));

	/* Check for success */
	if (KsvListIsValid) {
		/* Log */
		XHdcp1x_TxDebugLog(InstancePtr, "ksv list validated");

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_AUTHENTICATED;
	}
	else {
		/* Log */
		XHdcp1x_TxDebugLog(InstancePtr, "ksv list invalid");

		/* Update NextStatePtr */
		*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;
	}
}

/*****************************************************************************/
/**
* This function runs the "disabled" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunDisabledState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For enable */
		case XHDCP1X_EVENT_ENABLE:
			*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;
			if ((InstancePtr->Tx.Flags & XVPHY_FLAG_PHY_UP) == 0) {
				*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			}
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			InstancePtr->Tx.Flags &= ~XVPHY_FLAG_PHY_UP;
			break;

		/* For physical layer up */
		case XHDCP1X_EVENT_PHYUP:
			InstancePtr->Tx.Flags |= XVPHY_FLAG_PHY_UP;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "determine rx capable" state of the transmit state
* machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunDetermineRxCapableState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "exchange ksvs" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunExchangeKsvsState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "computations" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunComputationsState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* For poll */
		case XHDCP1X_EVENT_POLL:
			XHdcp1x_TxPollForComputations(InstancePtr,
					NextStatePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "validate-rx" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunValidateRxState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* For timeout */
		case XHDCP1X_EVENT_TIMEOUT:
			XHdcp1x_TxDebugLog(InstancePtr, "validate-rx timeout");
			XHdcp1x_TxValidateRx(InstancePtr, NextStatePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "authenticated" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunAuthenticatedState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For check */
		case XHDCP1X_EVENT_CHECK:
			*NextStatePtr = XHDCP1X_STATE_LINKINTEGRITYCHECK;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "link-integrity check" state of the transmit state
* machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunLinkIntegrityCheckState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* For poll */
		case XHDCP1X_EVENT_POLL:
			XHdcp1x_TxCheckLinkIntegrity(InstancePtr, NextStatePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "test-for-repeater" state of the transmit state
* machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunTestForRepeaterState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* For poll */
		case XHDCP1X_EVENT_POLL:
			XHdcp1x_TxTestForRepeater(InstancePtr, NextStatePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "wait-for-ready" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunWaitForReadyState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* For poll */
		case XHDCP1X_EVENT_POLL:
			XHdcp1x_TxPollForWaitForReady(InstancePtr,
					NextStatePtr);
			break;

		/* For timeout */
		case XHDCP1X_EVENT_TIMEOUT:
			XHdcp1x_TxDebugLog(InstancePtr,
					"wait-for-ready timeout");
			XHdcp1x_TxPollForWaitForReady(InstancePtr,
					NextStatePtr);
			if (*NextStatePtr == XHDCP1X_STATE_WAITFORREADY) {
				*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;
			}
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "read-ksv-list" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunReadKsvListState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function runs the "unauthenticated" state of the transmit state machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunUnauthenticatedState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For authenticate */
		case XHDCP1X_EVENT_AUTHENTICATE:
			*NextStatePtr = XHDCP1X_STATE_DETERMINERXCAPABLE;
			break;

		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer down */
		case XHDCP1X_EVENT_PHYDOWN:
			*NextStatePtr = XHDCP1X_STATE_PHYDOWN;
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}


/*****************************************************************************/
/**
* This function runs the "physical-layer-down" state of the transmit state
* machine.
*
* @param	InstancePtr is the transmitter instance.
* @param	Event is the event to process.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxRunPhysicalLayerDownState(XHdcp1x *InstancePtr,
		XHdcp1x_EventType Event, XHdcp1x_StateType *NextStatePtr)
{
	/* Which event? */
	switch (Event) {
		/* For disable */
		case XHDCP1X_EVENT_DISABLE:
			*NextStatePtr = XHDCP1X_STATE_DISABLED;
			break;

		/* For physical layer up */
		case XHDCP1X_EVENT_PHYUP:
			*NextStatePtr = XHDCP1X_STATE_UNAUTHENTICATED;
			if (InstancePtr->Tx.EncryptionMap != 0) {
				XHdcp1x_TxPostEvent(InstancePtr,
					XHDCP1X_EVENT_AUTHENTICATE);
			}
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function enters a state.
*
* @param	InstancePtr is the HDCP state machine.
* @param	State is the state to enter.
* @param	NextStatePtr is the next state.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxEnterState(XHdcp1x *InstancePtr, XHdcp1x_StateType State,
		XHdcp1x_StateType *NextStatePtr)
{
	/* Which state? */
	switch (State) {
		/* For the disabled state */
		case XHDCP1X_STATE_DISABLED:
			XHdcp1x_TxDisableState(InstancePtr);
			break;

		/* For determine rx capable */
		case XHDCP1X_STATE_DETERMINERXCAPABLE:
			InstancePtr->Tx.Flags |= XVPHY_FLAG_PHY_UP;
			XHdcp1x_TxSetCheckLinkState(InstancePtr, FALSE);
			XHdcp1x_TxDisableEncryptionState(InstancePtr);
			XHdcp1x_TxCheckRxCapable(InstancePtr, NextStatePtr);
			break;

		/* For the exchange ksvs state */
		case XHDCP1X_STATE_EXCHANGEKSVS:
			InstancePtr->Tx.StateHelper = 0;
			XHdcp1x_TxExchangeKsvs(InstancePtr, NextStatePtr);
			break;

		/* For the computations state */
		case XHDCP1X_STATE_COMPUTATIONS:
			XHdcp1x_TxStartComputations(InstancePtr, NextStatePtr);
			break;

		/* For the validate rx state */
		case XHDCP1X_STATE_VALIDATERX:
			InstancePtr->Tx.StateHelper = 0;
			XHdcp1x_TxStartTimer(InstancePtr, XVPHY_TMO_100MS);
			break;

		/* For the wait for ready state */
		case XHDCP1X_STATE_WAITFORREADY:
			InstancePtr->Tx.StateHelper = 0;
			XHdcp1x_TxStartTimer(InstancePtr,
					(5 * XVPHY_TMO_1SECOND));
			break;

		/* For the read ksv list state */
		case XHDCP1X_STATE_READKSVLIST:
			XHdcp1x_TxReadKsvList(InstancePtr, NextStatePtr);
			break;

		/* For the authenticated state */
		case XHDCP1X_STATE_AUTHENTICATED:
			InstancePtr->Tx.StateHelper = 0;
			XHdcp1x_TxEnableEncryptionState(InstancePtr);
			if (InstancePtr->Tx.PreviousState !=
					XHDCP1X_STATE_LINKINTEGRITYCHECK) {
				InstancePtr->Tx.Stats.AuthPassed++;
				XHdcp1x_TxSetCheckLinkState(InstancePtr, TRUE);
				XHdcp1x_TxDebugLog(InstancePtr,
					"authenticated");
			}
			break;

		/* For the link integrity check state */
		case XHDCP1X_STATE_LINKINTEGRITYCHECK:
			XHdcp1x_TxCheckLinkIntegrity(InstancePtr, NextStatePtr);
			break;

		/* For the unauthenticated state */
		case XHDCP1X_STATE_UNAUTHENTICATED:
			InstancePtr->Tx.Flags &= ~XVPHY_FLAG_IS_REPEATER;
			InstancePtr->Tx.Flags |= XVPHY_FLAG_PHY_UP;
			XHdcp1x_TxDisableEncryptionState(InstancePtr);
			break;

		/* For physical layer down */
		case XHDCP1X_STATE_PHYDOWN:
			InstancePtr->Tx.Flags &= ~XVPHY_FLAG_PHY_UP;
			XHdcp1x_TxDisableEncryptionState(InstancePtr);
			XHdcp1x_CipherDisable(InstancePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function exits a state.
*
* @param	InstancePtr is the HDCP state machine.
* @param	State is the state to exit.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxExitState(XHdcp1x *InstancePtr, XHdcp1x_StateType State)
{
	/* Which state? */
	switch (State) {
		/* For the disabled state */
		case XHDCP1X_STATE_DISABLED:
			XHdcp1x_TxEnableState(InstancePtr);
			break;

		/* For the computations state */
		case XHDCP1X_STATE_COMPUTATIONS:
			InstancePtr->Tx.StateHelper = 0;
			break;

		/* For the validate rx state */
		case XHDCP1X_STATE_VALIDATERX:
			XHdcp1x_TxStopTimer(InstancePtr);
			break;

		/* For the wait for ready state */
		case XHDCP1X_STATE_WAITFORREADY:
			XHdcp1x_TxStopTimer(InstancePtr);
			break;

		/* For the read ksv list state */
		case XHDCP1X_STATE_READKSVLIST:
			InstancePtr->Tx.StateHelper = 0;
			break;

		/* For physical layer down */
		case XHDCP1X_STATE_PHYDOWN:
			XHdcp1x_CipherEnable(InstancePtr);
			break;

		/* Otherwise */
		default:
			/* Do nothing */
			break;
	}
}

/*****************************************************************************/
/**
* This function drives a transmit state machine.
*
* @param	InstancePtr is the HDCP state machine.
* @param	Event is the event to process.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxDoTheState(XHdcp1x *InstancePtr, XHdcp1x_EventType Event)
{
	XHdcp1x_StateType NextState = InstancePtr->Tx.CurrentState;

	/* Which state? */
	switch (InstancePtr->Tx.CurrentState) {
		/* For the disabled state */
		case XHDCP1X_STATE_DISABLED:
			XHdcp1x_TxRunDisabledState(InstancePtr, Event,
								&NextState);
			break;

		/* For determine rx capable state */
		case XHDCP1X_STATE_DETERMINERXCAPABLE:
			XHdcp1x_TxRunDetermineRxCapableState(InstancePtr, Event,
								&NextState);
			break;

		/* For exchange ksvs state */
		case XHDCP1X_STATE_EXCHANGEKSVS:
			XHdcp1x_TxRunExchangeKsvsState(InstancePtr, Event,
								&NextState);
			break;

		/* For the computations state */
		case XHDCP1X_STATE_COMPUTATIONS:
			XHdcp1x_TxRunComputationsState(InstancePtr, Event,
								&NextState);
			break;

		/* For the validate rx state */
		case XHDCP1X_STATE_VALIDATERX:
			XHdcp1x_TxRunValidateRxState(InstancePtr, Event,
								&NextState);
			break;

		/* For the authenticated state */
		case XHDCP1X_STATE_AUTHENTICATED:
			XHdcp1x_TxRunAuthenticatedState(InstancePtr, Event,
								&NextState);
			break;

		/* For the link integrity check state */
		case XHDCP1X_STATE_LINKINTEGRITYCHECK:
			XHdcp1x_TxRunLinkIntegrityCheckState(InstancePtr, Event,
								&NextState);
			break;

		/* For the test for repeater state */
		case XHDCP1X_STATE_TESTFORREPEATER:
			XHdcp1x_TxRunTestForRepeaterState(InstancePtr, Event,
								&NextState);
			break;

		/* For the wait for ready state */
		case XHDCP1X_STATE_WAITFORREADY:
			XHdcp1x_TxRunWaitForReadyState(InstancePtr, Event,
								&NextState);
			break;

		/* For the reads ksv list state */
		case XHDCP1X_STATE_READKSVLIST:
			XHdcp1x_TxRunReadKsvListState(InstancePtr, Event,
								&NextState);
			break;

		/* For the unauthenticated state */
		case XHDCP1X_STATE_UNAUTHENTICATED:
			XHdcp1x_TxRunUnauthenticatedState(InstancePtr, Event,
								&NextState);
			break;

		/* For the physical layer down state */
		case XHDCP1X_STATE_PHYDOWN:
			XHdcp1x_TxRunPhysicalLayerDownState(InstancePtr, Event,
								&NextState);
			break;

		/* Otherwise */
		default:
			break;
	}

	/* Check for state change */
	while (InstancePtr->Tx.CurrentState != NextState) {
		/* Perform the state transition */
		XHdcp1x_TxExitState(InstancePtr, InstancePtr->Tx.CurrentState);
		InstancePtr->Tx.PreviousState = InstancePtr->Tx.CurrentState;
		InstancePtr->Tx.CurrentState = NextState;
		XHdcp1x_TxEnterState(InstancePtr, InstancePtr->Tx.CurrentState,
								&NextState);
	}
}

/*****************************************************************************/
/**
* This function processes the events pending on a state machine.
*
* @param	InstancePtr is the receiver instance.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
static void XHdcp1x_TxProcessPending(XHdcp1x *InstancePtr)
{
	/* Check for any pending events */
	if (InstancePtr->Tx.PendingEvents != 0) {
		u16 Pending = InstancePtr->Tx.PendingEvents;
		XHdcp1x_EventType Event = XHDCP1X_EVENT_NULL;

		/* Update InstancePtr */
		InstancePtr->Tx.PendingEvents = 0;

		/* Iterate through thePending */
		do {
			/* Check for a pending event */
			if ((Pending & 1u) != 0) {
				XHdcp1x_TxDoTheState(InstancePtr, Event);
			}

			/* Update for loop */
			Pending >>= 1;
			Event++;
		}
		while (Pending != 0);
	}
}

/*****************************************************************************/
/**
* This function converts from a state to a display string.
*
* @param	State is the state to convert.
*
* @return	The corresponding display string.
*
* @note		None.
*
******************************************************************************/
static const char *XHdcp1x_TxStateToString(XHdcp1x_StateType State)
{
	const char *String = NULL;

	/* Which state? */
	switch (State) {
		case XHDCP1X_STATE_DISABLED:
			String = "disabled";
			break;

		case XHDCP1X_STATE_DETERMINERXCAPABLE:
			String = "determine-rx-capable";
			break;

		case XHDCP1X_STATE_EXCHANGEKSVS:
			String = "exchange-ksvs";
			break;

		case XHDCP1X_STATE_COMPUTATIONS:
			String = "computations";
			break;

		case XHDCP1X_STATE_VALIDATERX:
			String = "validate-rx";
			break;

		case XHDCP1X_STATE_AUTHENTICATED:
			String = "authenticated";
			break;

		case XHDCP1X_STATE_LINKINTEGRITYCHECK:
			String = "link-integrity-check";
			break;

		case XHDCP1X_STATE_TESTFORREPEATER:
			String = "test-for-repeater";
			break;

		case XHDCP1X_STATE_WAITFORREADY:
			String = "wait-for-ready";
			break;

		case XHDCP1X_STATE_READKSVLIST:
			String = "read-ksv-list";
			break;

		case XHDCP1X_STATE_UNAUTHENTICATED:
			String = "unauthenticated";
			break;

		case XHDCP1X_STATE_PHYDOWN:
			String = "physical-layer-down";
			break;

		default:
			String = "???";
			break;
	}

	return (String);
}