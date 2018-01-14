/*
 * This file is part of the TREZOR project, https://trezor.io/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "protect.h"
#include "storage.h"
#include "messages.h"
#include "usb.h"
#include "oled.h"
#include "buttons.h"
#include "pinmatrix.h"
#include "fsm.h"
#include "layout2.h"
#include "util.h"
#include "debug.h"
#include "gettext.h"
#include "rng.h"

#define MAX_WRONG_PINS 15

bool protectAbortedByInitialize = false;

bool protectButton(ButtonRequestType type, bool confirm_only)
{
	ButtonRequest resp;
	bool result = false;
	bool acked = false;
#if DEBUG_LINK
	bool debug_decided = false;
#endif

	memset(&resp, 0, sizeof(ButtonRequest));
	resp.has_code = true;
	resp.code = type;
	usbTiny(1);
	buttonUpdate(); // Clear button state
	msg_write(MessageType_MessageType_ButtonRequest, &resp);

	for (;;) {
		usbPoll();

		// check for ButtonAck
		if (msg_tiny_id == MessageType_MessageType_ButtonAck) {
			msg_tiny_id = 0xFFFF;
			acked = true;
		}

		// button acked - check buttons
		if (acked) {
			usbSleep(5);
			buttonUpdate();
			if (button.YesUp) {
				result = true;
				break;
			}
			if (!confirm_only && button.NoUp) {
				result = false;
				break;
			}
		}

		// check for Cancel / Initialize
		if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
			if (msg_tiny_id == MessageType_MessageType_Initialize) {
				protectAbortedByInitialize = true;
			}
			msg_tiny_id = 0xFFFF;
			result = false;
			break;
		}

#if DEBUG_LINK
		// check DebugLink
		if (msg_tiny_id == MessageType_MessageType_DebugLinkDecision) {
			msg_tiny_id = 0xFFFF;
			DebugLinkDecision *dld = (DebugLinkDecision *)msg_tiny;
			result = dld->yes_no;
			debug_decided = true;
		}

		if (acked && debug_decided) {
			break;
		}

		if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
			msg_tiny_id = 0xFFFF;
			fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
		}
#endif
	}

	usbTiny(0);

	return result;
}

const char *requestPin(PinMatrixRequestType type, const char *text)
{
	PinMatrixRequest resp;
	memset(&resp, 0, sizeof(PinMatrixRequest));
	resp.has_type = true;
	resp.type = type;
	usbTiny(1);
	msg_write(MessageType_MessageType_PinMatrixRequest, &resp);
	pinmatrix_start(text);
	for (;;) {
		usbPoll();
		if (msg_tiny_id == MessageType_MessageType_PinMatrixAck) {
			msg_tiny_id = 0xFFFF;
			PinMatrixAck *pma = (PinMatrixAck *)msg_tiny;
			pinmatrix_done(pma->pin); // convert via pinmatrix
			usbTiny(0);
			return pma->pin;
		}
		if (msg_tiny_id == MessageType_MessageType_Cancel || msg_tiny_id == MessageType_MessageType_Initialize) {
			pinmatrix_done(0);
			if (msg_tiny_id == MessageType_MessageType_Initialize) {
				protectAbortedByInitialize = true;
			}
			msg_tiny_id = 0xFFFF;
			usbTiny(0);
			return 0;
		}
#if DEBUG_LINK
		if (msg_tiny_id == MessageType_MessageType_DebugLinkGetState) {
			msg_tiny_id = 0xFFFF;
			fsm_msgDebugLinkGetState((DebugLinkGetState *)msg_tiny);
		}
#endif
	}
}

static void protectCheckMaxTry(uint32_t wait) {
	if (wait < (1 << MAX_WRONG_PINS))
		return;

	storage_wipe();
	layoutDialog(&bmp_icon_error, NULL, NULL, NULL, _("Too many wrong PIN"), _("attempts. Storage has"), _("been wiped."), NULL, _("Please unplug"), _("the device."));
	for (;;) {} // loop forever
}

bool protectPin(bool use_cached)
{
	if (!storage.has_pin || storage.pin[0] == 0 || (use_cached && session_isPinCached())) {
		return true;
	}
	uint32_t *fails = storage_getPinFailsPtr();
	uint32_t wait = ~*fails;
	protectCheckMaxTry(wait);
	usbTiny(1);
	while (wait > 0) {
		// convert wait to secstr string
		char secstrbuf[20];
		strlcpy(secstrbuf, _("________0 seconds"), sizeof(secstrbuf));
		char *secstr = secstrbuf + 9;
		uint32_t secs = wait;
		while (secs > 0 && secstr >= secstrbuf) {
			secstr--;
			*secstr = (secs % 10) + '0';
			secs /= 10;
		}
		if (wait == 1) {
			secstrbuf[16] = 0;
		}
		layoutDialog(&bmp_icon_info, NULL, NULL, NULL, _("Wrong PIN entered"), NULL, _("Please wait"), secstr, _("to continue ..."), NULL);
		// wait one second
		usbSleep(1000);
		if (msg_tiny_id == MessageType_MessageType_Initialize) {
			protectAbortedByInitialize = true;
			msg_tiny_id = 0xFFFF;
			usbTiny(0);
			fsm_sendFailure(FailureType_Failure_PinCancelled, NULL);
			return false;
		}
		wait--;
	}
	usbTiny(0);
	const char *pin;
	pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_Current, _("Please enter current PIN:"));
	if (!pin) {
		fsm_sendFailure(FailureType_Failure_PinCancelled, NULL);
		return false;
	}
	if (!storage_increasePinFails(fails)) {
		fsm_sendFailure(FailureType_Failure_PinInvalid, NULL);
		return false;
	}
	if (storage_containsPin(pin)) {
		session_cachePin();
		storage_resetPinFails(fails);
		return true;
	} else {
		protectCheckMaxTry(~*fails);
		fsm_sendFailure(FailureType_Failure_PinInvalid, NULL);
		return false;
	}
}

bool protectChangePin(void)
{
	const char *pin;
	char pin1[17], pin2[17];
	pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_NewFirst, _("Please enter new PIN:"));
	if (!pin) {
		return false;
	}
	strlcpy(pin1, pin, sizeof(pin1));
	pin = requestPin(PinMatrixRequestType_PinMatrixRequestType_NewSecond, _("Please re-enter new PIN:"));
	if (!pin) {
		return false;
	}
	strlcpy(pin2, pin, sizeof(pin2));
	if (strcmp(pin1, pin2) == 0) {
		storage_setPin(pin1);
		return true;
	} else {
		return false;
	}
}

void buttonCheckRepeat(bool *yes, bool *no, bool *confirm)
{
	*yes = false;
	*no = false;
	*confirm = false;

	const int Threshold0 = 20;
	const int Thresholds[] = { Threshold0, 80, 20 };
	const int MaxThresholdLevel = sizeof(Thresholds)/sizeof(Thresholds[0]) - 1;

	static int yesthreshold = Threshold0;
	static int nothreshold = Threshold0;

	static int yeslevel = 0;
	static int nolevel = 0;

	static bool both = false;

	usbSleep(5);
	buttonUpdate();
	
	if (both)
	{
		if (!button.YesDown && !button.NoDown)
		{
			both = false;
			yeslevel = 0;
			nolevel = 0;
			yesthreshold = Thresholds[0];
			nothreshold = Thresholds[0];
		}
	}
	else if ((button.YesDown && button.NoDown)
		|| (button.YesUp && button.NoDown)
		|| (button.YesDown && button.NoUp)
		|| (button.YesUp && button.NoUp))
	{
		if (!yeslevel && !nolevel)
		{
			both = true;
			*confirm = true;
		}
	}
	else
	{
		if (button.YesUp)
		{
			if (!yeslevel)
				*yes = true;
			yeslevel = 0;
			yesthreshold = Thresholds[0];
		}
		else if (button.YesDown >= yesthreshold)
		{
			if (yeslevel < MaxThresholdLevel)
				yeslevel++;
			yesthreshold += Thresholds[yeslevel];
			*yes = true;
		}
		if (button.NoUp)
		{
			if (!nolevel)
				*no = true;
			nolevel = 0;
			nothreshold = Thresholds[0];
		}
		else if (button.NoDown >= nothreshold)
		{
			if (nolevel < MaxThresholdLevel)
				nolevel++;
			nothreshold += Thresholds[nolevel];
			*no = true;
		}
	}
}

#define Backspace "\x08"
#define Space "\x09"
#define Done "\x06"
#define Back "\x0b\x42\x41\x43\x4b"

#define CaretShowThreshold 80
#define CaretCycle (CaretShowThreshold * 2)

#define InputDone -2
#define InputBack -1

void confirmedDoNothing(char *passphrase, int *passphrasecharindex, char ch)
{
	UNUSED(passphrase);
	UNUSED(passphrasecharindex);
	UNUSED(ch);
}

void confirmedAcceptChar(char *passphrase, int *passphrasecharindex, char ch)
{
	if (*passphrasecharindex < 50)
	{
		passphrase[*passphrasecharindex] = ch;
		++(*passphrasecharindex);
	}
}

int inputPassphraseNavigation(char *passphrase, int *passphrasecharindex, const char entries[][12], int numtotal, int numscreen, int padding, int numexcluded, void (*confirmed)(char *, int *, char), int *caret)
{
	int numrandom = numtotal - numexcluded;
	int entryindex = random32() % numrandom;

	for (; ; *caret = (*caret + 1) % CaretCycle)
	{
		bool yes, no, confirm;
		buttonCheckRepeat(&yes, &no, &confirm);

		if (confirm)
		{
			if (entries[entryindex][0] == Backspace[0])
			{
				if (*passphrasecharindex > 0)
				{
					--(*passphrasecharindex);
					passphrase[*passphrasecharindex] = 0;
				}
				if (*passphrasecharindex == 49)
					return entryindex;
			}
			else if (entries[entryindex][0] == Done[0])
			{
				return InputDone;
			}
			else if (entries[entryindex][0] == Back[0])
			{
				return InputBack;
			}
			else
			{
				if (confirmed)
					(*confirmed)(passphrase, passphrasecharindex, entries[entryindex][0]);
				return entryindex;
			}

			entryindex = random32() % numrandom;
		}
		else
		{
			if (yes)
				entryindex = (entryindex + 1) % numtotal;
			if (no)
				entryindex = (entryindex - 1 + numtotal) % numtotal;
		}

		layoutScroll(passphrase, numtotal, numscreen, entryindex, entries, padding);
		if (*caret < CaretShowThreshold)
			oledDrawCaret();
	}
}

void inputPassphrase(char *passphrase)
{
	#define NumMain 12
	#define NumSub 10
	#define NumPerSub 14
	#define NumCapped 4
	
	const char MainEntries[NumMain][12] = {
		"abcdefghi",
		"jklmnopqr",
		"stuvwxyz\x09",
		"ABCDEFGHI",
		"JKLMNOPQR",
		"STUVWXYZ\x09",
		"1234567890",
		"!@#$\x25^&*()",
		"`-=[]\\;',./",
		"~_+{}|:\"<>?",
		Backspace,
		Done,
	};
	const char SubEntries[NumSub][NumPerSub][12] = {
		{ "a", "b", "c", "d", "e", "f", "g", "h", "i", Backspace, Done, Back, "", "" },
		{ "j", "k", "l", "m", "n", "o", "p", "q", "r", Backspace, Done, Back, "", "" },
		{ "s", "t", "u", "v", "w", "x", "y", "z", Space, Backspace, Done, Back, "", "" },
		{ "A", "B", "C", "D", "E", "F", "G", "H", "I", Backspace, Done, Back, "", "" },
		{ "J", "K", "L", "M", "N", "O", "P", "Q", "R", Backspace, Done, Back, "", "" },
		{ "S", "T", "U", "V", "W", "X", "Y", "Z", Space, Backspace, Done, Back, "", "" },
		{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", Backspace, Done, Back, "" },
		{ "!", "@", "#", "$", "\x25", "^", "&", "*", "(", ")", Backspace, Done, Back, "" },
		{ "`", "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/", Backspace, Done, Back },
		{ "~", "_", "+", "{", "}", "|", ":", "\"", "<", ">", "?", Backspace, Done, Back }
	};
	const char CappedEntries[NumCapped][12] = {
		Backspace,
		Done,
		Backspace,
		Done,
	};

	int nums[NumSub];
	for (int i = 0; i < NumSub; i++)
	{
		int j;
		for (j = 0; j < NumPerSub && SubEntries[i][j] && SubEntries[i][j][0]; j++);
		nums[i] = j;
	}

	buttonUpdate();

	int passphrasecharindex = strlen(passphrase);
	int caret = 0;

	if (passphrasecharindex >= 50 && InputDone == inputPassphraseNavigation(passphrase, &passphrasecharindex, CappedEntries, NumCapped, 3, 6, 0, confirmedDoNothing, &caret))
		return;

	int mainentryindex, subentryindex;
	for (;;)
	{
		mainentryindex = inputPassphraseNavigation(passphrase, &passphrasecharindex, MainEntries, NumMain, 3, 0, 2, confirmedDoNothing, &caret);
		if (mainentryindex == InputDone)
			return;
		mainentryindex = mainentryindex < 0 ? 0 : mainentryindex; // for the warning
		for (;;)
		{
			subentryindex = inputPassphraseNavigation(passphrase, &passphrasecharindex, SubEntries[mainentryindex], nums[mainentryindex], 5, 4, 3, confirmedAcceptChar, &caret);
			if (subentryindex == InputDone)
				return;
			if (subentryindex == InputBack)
				break;
			if (passphrasecharindex >= 50 && InputDone == inputPassphraseNavigation(passphrase, &passphrasecharindex, CappedEntries, NumCapped, 3, 6, 0, confirmedDoNothing, &caret))
				return;
		}
	}
}

bool checkPassphrase(const char *passphrase)
{
	layoutCheckPassphrase(passphrase);

	buttonUpdate();

	for(;;)
	{
		usbSleep(5);
		buttonUpdate();
		if (button.YesUp)
			return true;
		if (button.NoUp)
			return false;
	}
}

void waitForYesButton(void)
{
	buttonUpdate();

	for(;;)
	{
		usbSleep(5);
		buttonUpdate();
		if (button.YesUp)
			break;
	}
}

bool protectPassphrase(void)
{
	static char CONFIDENTIAL passphrase[51];

	if (!storage.has_passphrase_protection || !storage.passphrase_protection || session_isPassphraseCached()) {
		return true;
	}
	
	memset(passphrase, 0, 51);
	buttonUpdate();

	layoutDialog(NULL, NULL, _("Next"), NULL, _("You are about to enter"), _("the passphrase."), _("Select how many times"), _("you'd like to do it."), NULL, NULL);
	waitForYesButton();
	layoutSwipe();

	layoutDialog(NULL, _("Once"), _("Twice"), NULL, _("If you are creating a new"), _("wallet or restoring an"), _("unused one, it is advised"), _("that you select Twice."), NULL, NULL);
	for(;;)
	{
		usbSleep(5);
		buttonUpdate();
		if (button.YesUp || button.NoUp)
			break;
	}
	layoutSwipe();

	bool once = button.NoUp;

	for (;;)
	{
		layoutDialog(NULL, NULL, _("Next"), NULL, _("Enter the passphrase"), _("on the next screen."), _("- Single button: scroll."), _("- Hold: auto-scroll."), _("- Both buttons: confirm."), NULL);
		waitForYesButton();
		layoutSwipe();

		for (;;)
		{
			inputPassphrase(passphrase);
			layoutSwipe();

			if (checkPassphrase(passphrase))
				break;

			oledSwipeRight();
		}

		layoutSwipe();

		if (once)
			break;
		
		static char CONFIDENTIAL passphrase2[51];

		memset(passphrase2, 0, 51);

		layoutDialog(NULL, NULL, _("Next"), NULL, _("Re-enter the passphrase."), NULL, NULL, NULL, NULL, NULL);
		waitForYesButton();
		layoutSwipe();

		for (;;)
		{
			inputPassphrase(passphrase2);
			layoutSwipe();

			if (checkPassphrase(passphrase2))
				break;

			oledSwipeRight();
		}

		layoutSwipe();

		if (strcmp(passphrase, passphrase2) == 0)
			break;

		layoutDialog(NULL, NULL, _("Next"), NULL, _("Passphrases do not"), _("match. Try again."), NULL, NULL, NULL, NULL);
		waitForYesButton();
		layoutSwipe();

		memset(passphrase, 0, 51);
		memset(passphrase2, 0, 51);
	}

	for (int i = 0; i < 51 && passphrase[i]; ++i)
		if (passphrase[i] == 0x09) // Space
			passphrase[i] = ' ';
			
	session_cachePassphrase(passphrase);

	layoutHome();

	return true;
}
