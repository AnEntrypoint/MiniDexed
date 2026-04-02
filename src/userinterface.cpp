//
// userinterface.cpp
//
// MiniDexed - Dexed FM synthesizer for bare metal Raspberry Pi
// Copyright (C) 2022  The MiniDexed Team
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "userinterface.h"
#include "minidexed.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/startup.h>
#include <circle/devicenameservice.h>
#include <string.h>
#include <assert.h>

LOGMODULE ("ui");

CUserInterface::CUserInterface (CMiniDexed *pMiniDexed, CGPIOManager *pGPIOManager, CI2CMaster *pI2CMaster, CSPIMaster *pSPIMaster, CConfig *pConfig)
:	m_pMiniDexed (pMiniDexed),
	m_pGPIOManager (pGPIOManager),
	m_pI2CMaster (pI2CMaster),
	m_pSPIMaster (pSPIMaster),
	m_pConfig (pConfig),
	m_pLCD (0),
	m_pLCDBuffered (0),
	m_pCFA635Device (0),
	m_bCFA635Enabled (false),
	m_bCFA635Ready (false),
	m_nCFAReadBufferLen (0),
	m_pUIButtons (0),
	m_pRotaryEncoder (0),
	m_bSwitchPressed (false),
	m_Menu (this, pMiniDexed, pConfig)
{
	m_CFALine0[0] = '\0';
	m_CFALine1[0] = '\0';
}

CUserInterface::~CUserInterface (void)
{
	delete m_pRotaryEncoder;
	delete m_pUIButtons;
	delete m_pLCDBuffered;
	delete m_pLCD;
}

bool CUserInterface::Initialize (void)
{
	assert (m_pConfig);

	if (m_pConfig->GetLCDEnabled ())
	{
		m_bCFA635Enabled = m_pConfig->GetCFA635Enabled ();
		if (m_bCFA635Enabled)
		{
			LOGDBG ("LCD: CFA635 serial backend enabled");
		}

		unsigned i2caddr = m_pConfig->GetLCDI2CAddress ();
		unsigned ssd1306addr = m_pConfig->GetSSD1306LCDI2CAddress ();
		bool st7789 = m_pConfig->GetST7789Enabled ();
		if (m_bCFA635Enabled)
		{
			// Handled dynamically in ProcessCFA635 once USB serial device appears.
		}
		else if (ssd1306addr != 0) {
			m_pSSD1306 = new CSSD1306Device (m_pConfig->GetSSD1306LCDWidth (), m_pConfig->GetSSD1306LCDHeight (),
											 m_pI2CMaster, ssd1306addr,
											 m_pConfig->GetSSD1306LCDRotate (), m_pConfig->GetSSD1306LCDMirror ());
			if (!m_pSSD1306->Initialize ())
			{
				LOGDBG("LCD: SSD1306 initialization failed");
				return false;
			}
			LOGDBG ("LCD: SSD1306");
			m_pLCD = m_pSSD1306;
		}
		else if (st7789)
		{
			if (m_pSPIMaster == nullptr)
			{
				LOGDBG("LCD: ST7789 Enabled but SPI Initialisation Failed");
				return false;
			}

			unsigned long nSPIClock = 1000 * m_pConfig->GetSPIClockKHz();
			unsigned nSPIMode = m_pConfig->GetSPIMode();
			unsigned nCPHA = (nSPIMode & 1) ? 1 : 0;
			unsigned nCPOL = (nSPIMode & 2) ? 1 : 0;
			LOGDBG("SPI: CPOL=%u; CPHA=%u; CLK=%u",nCPOL,nCPHA,nSPIClock);
			m_pST7789Display = new CST7789Display (m_pSPIMaster,
							m_pConfig->GetST7789Data(),
							m_pConfig->GetST7789Reset(),
							m_pConfig->GetST7789Backlight(),
							m_pConfig->GetST7789Width(),
							m_pConfig->GetST7789Height(),
							nCPOL, nCPHA, nSPIClock,
							m_pConfig->GetST7789Select());
			if (m_pST7789Display->Initialize())
			{
				m_pST7789Display->SetRotation (m_pConfig->GetST7789Rotation());
				bool bLargeFont = !(m_pConfig->GetST7789SmallFont());
				m_pST7789 = new CST7789Device (m_pSPIMaster, m_pST7789Display, m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows (), Font8x16, bLargeFont, bLargeFont);
				if (m_pST7789->Initialize())
				{
					LOGDBG ("LCD: ST7789");
					m_pLCD = m_pST7789;
				}
				else
				{
					LOGDBG ("LCD: Failed to initalize ST7789 character device");
					delete (m_pST7789);
					delete (m_pST7789Display);
					m_pST7789 = nullptr;
					m_pST7789Display = nullptr;
					return false;
				}
			}
			else
			{
				LOGDBG ("LCD: Failed to initialize ST7789 display");
				delete (m_pST7789Display);
				m_pST7789Display = nullptr;
				return false;
			}
		}
		else if (i2caddr == 0)
		{
			m_pHD44780 = new CHD44780Device (m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows (),
							 m_pConfig->GetLCDPinData4 (),
							 m_pConfig->GetLCDPinData5 (),
							 m_pConfig->GetLCDPinData6 (),
							 m_pConfig->GetLCDPinData7 (),
							 m_pConfig->GetLCDPinEnable (),
							 m_pConfig->GetLCDPinRegisterSelect (),
							 m_pConfig->GetLCDPinReadWrite ());
			if (!m_pHD44780->Initialize ())
			{
				LOGDBG("LCD: HD44780 initialization failed");
				return false;
			}
			LOGDBG ("LCD: HD44780");
			m_pLCD = m_pHD44780;
		}
		else
		{
			m_pHD44780 = new CHD44780Device (m_pI2CMaster, i2caddr,
							m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows ());
			if (!m_pHD44780->Initialize ())
			{
				LOGDBG("LCD: HD44780 (I2C) initialization failed");
				return false;
			}
			LOGDBG ("LCD: HD44780 I2C");
			m_pLCD = m_pHD44780;
		}
		if (m_pLCD)
		{
			m_pLCDBuffered = new CWriteBufferDevice (m_pLCD);
			assert (m_pLCDBuffered);
			// clear sceen and go to top left corner
			LCDWrite ("\x1B[H\x1B[J");		// cursor home and clear screen
			LCDWrite ("\x1B[?25l\x1B""d+");		// cursor off, autopage mode
			LCDWrite ("MiniDexed\nLoading...");
			m_pLCDBuffered->Update ();
		}

		LOGDBG ("LCD initialized");
	}

	m_pUIButtons = new CUIButtons (	m_pConfig );
	assert (m_pUIButtons);

	if (!m_pUIButtons->Initialize ())
	{
		return false;
	}

	m_pUIButtons->RegisterEventHandler (UIButtonsEventStub, this);
	UISetMIDIButtonChannel (m_pConfig->GetMIDIButtonCh ());

	LOGDBG ("Button User Interface initialized");

	if (m_pConfig->GetEncoderEnabled ())
	{
		m_pRotaryEncoder = new CKY040 (m_pConfig->GetEncoderPinClock (),
					       m_pConfig->GetEncoderPinData (),
					       m_pConfig->GetButtonPinShortcut (),
					       m_pGPIOManager,
					       m_pConfig->GetEncoderDetents ());
		assert (m_pRotaryEncoder);

		if (!m_pRotaryEncoder->Initialize ())
		{
			return false;
		}

		m_pRotaryEncoder->RegisterEventHandler (EncoderEventStub, this);

		LOGDBG ("Rotary encoder initialized");
	}

	m_Menu.EventHandler (CUIMenu::MenuEventUpdate);

	return true;
}

void CUserInterface::Process (void)
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Update ();
	}
	ProcessCFA635 ();
	if (m_pUIButtons)
	{
		m_pUIButtons->Update();
	}
}

void CUserInterface::ParameterChanged (void)
{
	m_Menu.EventHandler (CUIMenu::MenuEventUpdateParameter);
}

void CUserInterface::DisplayChanged (void)
{
	m_Menu.EventHandler (CUIMenu::MenuEventUpdate);
}

void CUserInterface::DisplayWrite (const char *pMenu, const char *pParam, const char *pValue,
				   bool bArrowDown, bool bArrowUp)
{
	assert (pMenu);
	assert (pParam);
	assert (pValue);

	CString Msg ("\x1B[H\E[?25l");		// cursor home and off

	// first line
	Msg.Append (pParam);

	size_t nLen = strlen (pParam) + strlen (pMenu);
	if (nLen < m_pConfig->GetLCDColumns ())
	{
		for (unsigned i = m_pConfig->GetLCDColumns ()-nLen; i > 0; i--)
		{
			Msg.Append (" ");
		}
	}

	Msg.Append (pMenu);

	// second line
	CString Value (" ");
	if (bArrowDown)
	{
		Value = "<";			// arrow left character
	}

	Value.Append (pValue);

	if (bArrowUp)
	{
		if (Value.GetLength () < m_pConfig->GetLCDColumns ()-1)
		{
			for (unsigned i = m_pConfig->GetLCDColumns ()-Value.GetLength ()-1; i > 0; i--)
			{
				Value.Append (" ");
			}
		}

		Value.Append (">");		// arrow right character
	}

	Msg.Append (Value);

	if (Value.GetLength () < m_pConfig->GetLCDColumns ())
	{
		Msg.Append ("\x1B[K");		// clear end of line
	}

	if (m_bCFA635Enabled)
	{
		unsigned cols = m_pConfig->GetLCDColumns ();
		if (cols > 40) cols = 40;
		unsigned i = 0;
		for (; i < cols && pParam[i] != '\0'; i++) m_CFALine0[i] = pParam[i];
		for (; i + strlen (pMenu) < cols && i < cols; i++) m_CFALine0[i] = ' ';
		for (unsigned j = 0; i < cols && pMenu[j] != '\0'; i++, j++) m_CFALine0[i] = pMenu[j];
		m_CFALine0[cols] = '\0';

		for (i = 0; i < cols; i++) m_CFALine1[i] = ' ';
		unsigned pos = 0;
		if (bArrowDown && pos < cols) m_CFALine1[pos++] = '<';
		for (unsigned j = 0; pValue[j] != '\0' && pos < cols; j++, pos++) m_CFALine1[pos] = pValue[j];
		if (bArrowUp && cols > 0) m_CFALine1[cols-1] = '>';
		m_CFALine1[cols] = '\0';
	}

	LCDWrite (Msg);
}

void CUserInterface::LCDWrite (const char *pString)
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Write (pString, strlen (pString));
	}
}

u16 CUserInterface::GetCFA635CRC (const u8 *pData, unsigned nLength)
{
	u16 crc = 0xFFFF;
	for (unsigned i = 0; i < nLength; i++)
	{
		u8 data = pData[i];
		for (unsigned j = 0; j < 8; j++)
		{
			if ((crc ^ data) & 0x01)
			{
				crc >>= 1;
				crc ^= 0x8408;
			}
			else
			{
				crc >>= 1;
			}
			data >>= 1;
		}
	}
	return (u16) ~crc;
}

bool CUserInterface::SendCFA635Command (u8 type, const u8 *pData, u8 nLength)
{
	if (!m_pCFA635Device || !m_bCFA635Ready)
	{
		return false;
	}

	u8 packet[2 + 22 + 2];
	packet[0] = type;
	packet[1] = nLength;
	for (u8 i = 0; i < nLength; i++)
	{
		packet[2+i] = pData ? pData[i] : 0;
	}
	u16 crc = GetCFA635CRC (packet, (unsigned) (2 + nLength));
	packet[2+nLength] = (u8) (crc & 0xFF);
	packet[3+nLength] = (u8) ((crc >> 8) & 0xFF);

	int nWritten = m_pCFA635Device->Write (packet, (unsigned) (4 + nLength));
	return nWritten == (int) (4 + nLength);
}

bool CUserInterface::SendCFA635Text (u8 row, u8 col, const char *pText)
{
	if (!pText)
	{
		return false;
	}

	const unsigned nColumns = m_pConfig->GetLCDColumns ();
	if (nColumns == 0)
	{
		return false;
	}

	u8 payload[22];
	payload[0] = col;
	payload[1] = row;
	unsigned n = 0;
	while (pText[n] && n < nColumns && n < 20)
	{
		payload[2+n] = (u8) pText[n];
		n++;
	}
	while (n < nColumns && n < 20)
	{
		payload[2+n] = ' ';
		n++;
	}
	return SendCFA635Command (0x1F, payload, (u8) (2+n));
}

void CUserInterface::HandleCFA635KeyActivity (u8 keyCode)
{
	switch (keyCode)
	{
	case 1: // UP press
		InjectButtonEvent (CUIButton::BtnEventBack);
		break;
	case 2: // DOWN press
		InjectButtonEvent (CUIButton::BtnEventSelect);
		break;
	case 3: // LEFT press
		InjectButtonEvent (CUIButton::BtnEventPrev);
		break;
	case 4: // RIGHT press
		InjectButtonEvent (CUIButton::BtnEventNext);
		break;
	case 5: // ENTER press
		InjectButtonEvent (CUIButton::BtnEventSelect);
		break;
	case 6: // EXIT press
		InjectButtonEvent (CUIButton::BtnEventHome);
		break;
	default:
		break;
	}
}

void CUserInterface::ProcessCFA635 (void)
{
	if (!m_bCFA635Enabled)
	{
		return;
	}

	if (!m_pCFA635Device)
	{
		const char *pNames[] = {
			m_pConfig->GetCFA635SerialDevice (),
			"CFA635-USB", "utty1", "utty2", "utty3",
			"ttyACM1", "ttyACM2", "ttyUSB1", "ttyUSB2"
		};
		for (unsigned i = 0; i < sizeof (pNames) / sizeof (pNames[0]); i++)
		{
			if (!pNames[i] || !pNames[i][0])
			{
				continue;
			}
			m_pCFA635Device = CDeviceNameService::Get ()->GetDevice (pNames[i], FALSE);
			if (m_pCFA635Device)
			{
				LOGNOTE ("LCD: CFA635 connected on %s", pNames[i]);
				m_bCFA635Ready = true;
				const u8 backlight = 100;
				SendCFA635Command (0x0E, &backlight, 1);
				SendCFA635Command (0x06, 0, 0);
				const u8 keyReportMask = 0x3F;
				SendCFA635Command (0x17, &keyReportMask, 1);
				break;
			}
		}
	}

	if (!m_pCFA635Device || !m_bCFA635Ready)
	{
		return;
	}

	if (m_CFALine0[0] != '\0' || m_CFALine1[0] != '\0')
	{
		if (m_CFALine0[0] != '\0')
		{
			SendCFA635Text (0, 0, m_CFALine0);
			m_CFALine0[0] = '\0';
		}
		if (m_CFALine1[0] != '\0')
		{
			SendCFA635Text (1, 0, m_CFALine1);
			m_CFALine1[0] = '\0';
		}
	}

	int nRead = m_pCFA635Device->Read (m_CFAReadBuffer + m_nCFAReadBufferLen,
					    sizeof (m_CFAReadBuffer) - m_nCFAReadBufferLen);
	if (nRead <= 0)
	{
		return;
	}
	m_nCFAReadBufferLen += (unsigned) nRead;

	while (m_nCFAReadBufferLen >= 4)
	{
		u8 type = m_CFAReadBuffer[0];
		u8 len = m_CFAReadBuffer[1];
		unsigned packetLen = 4 + len;
		if (packetLen > sizeof (m_CFAReadBuffer))
		{
			m_nCFAReadBufferLen = 0;
			return;
		}
		if (m_nCFAReadBufferLen < packetLen)
		{
			break;
		}
		u16 crcRx = (u16) m_CFAReadBuffer[2+len] | ((u16) m_CFAReadBuffer[3+len] << 8);
		u16 crcCalc = GetCFA635CRC (m_CFAReadBuffer, 2+len);
		if (crcRx == crcCalc && type == 0x80 && len == 1)
		{
			HandleCFA635KeyActivity (m_CFAReadBuffer[2]);
		}
		memmove (m_CFAReadBuffer, m_CFAReadBuffer + packetLen, m_nCFAReadBufferLen - packetLen);
		m_nCFAReadBufferLen -= packetLen;
	}
}

void CUserInterface::EncoderEventHandler (CKY040::TEvent Event)
{
	switch (Event)
	{
	case CKY040::EventSwitchDown:
		m_bSwitchPressed = true;
		break;

	case CKY040::EventSwitchUp:
		m_bSwitchPressed = false;
		break;

	case CKY040::EventClockwise:
		if (m_bSwitchPressed) {
			// We must reset the encoder switch button to prevent events from being
			// triggered after the encoder is rotated
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepUp);

		}
		else {
			m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
		}
		break;

	case CKY040::EventCounterclockwise:
		if (m_bSwitchPressed) {
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepDown);
		}
		else {
			m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
		}
		break;

	case CKY040::EventSwitchHold:
		if (m_pRotaryEncoder->GetHoldSeconds () >= 120)
		{
			delete m_pLCD;		// reset LCD

			reboot ();
		}
		break;

	default:
		break;
	}
}

void CUserInterface::EncoderEventStub (CKY040::TEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *> (pParam);
	assert (pThis != 0);

	pThis->EncoderEventHandler (Event);
}

void CUserInterface::UIButtonsEventHandler (CUIButton::BtnEvent Event)
{
	InjectButtonEvent (Event);
}

void CUserInterface::InjectButtonEvent (CUIButton::BtnEvent Event)
{
	switch (Event)
	{
	case CUIButton::BtnEventPrev:
		m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
		break;

	case CUIButton::BtnEventNext:
		m_Menu.EventHandler (CUIMenu::MenuEventStepUp);
		break;

	case CUIButton::BtnEventBack:
		m_Menu.EventHandler (CUIMenu::MenuEventBack);
		break;

	case CUIButton::BtnEventSelect:
		m_Menu.EventHandler (CUIMenu::MenuEventSelect);
		break;

	case CUIButton::BtnEventHome:
		m_Menu.EventHandler (CUIMenu::MenuEventHome);
		break;

	case CUIButton::BtnEventPgmUp:
		m_Menu.EventHandler (CUIMenu::MenuEventPgmUp);
		break;

	case CUIButton::BtnEventPgmDown:
		m_Menu.EventHandler (CUIMenu::MenuEventPgmDown);
		break;

	case CUIButton::BtnEventBankUp:
		m_Menu.EventHandler (CUIMenu::MenuEventBankUp);
		break;

	case CUIButton::BtnEventBankDown:
		m_Menu.EventHandler (CUIMenu::MenuEventBankDown);
		break;

	case CUIButton::BtnEventTGUp:
		m_Menu.EventHandler (CUIMenu::MenuEventTGUp);
		break;

	case CUIButton::BtnEventTGDown:
		m_Menu.EventHandler (CUIMenu::MenuEventTGDown);
		break;

	default:
		break;
	}
}

void CUserInterface::UIButtonsEventStub (CUIButton::BtnEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *> (pParam);
	assert (pThis != 0);

	pThis->UIButtonsEventHandler (Event);
}

void CUserInterface::UIMIDICmdHandler (unsigned nMidiCh, unsigned nMidiType, unsigned nMidiData1, unsigned nMidiData2)
{
	if (m_nMIDIButtonCh == CMIDIDevice::Disabled)
	{
		// MIDI buttons are not enabled
		return;
	}
	if ((m_nMIDIButtonCh != nMidiCh) && (m_nMIDIButtonCh != CMIDIDevice::OmniMode))
	{
		// Message not on the MIDI Button channel and MIDI buttons not in OMNI mode
		return;
	}
	
	if (m_pUIButtons)
	{
		m_pUIButtons->BtnMIDICmdHandler (nMidiType, nMidiData1, nMidiData2);
	}
}

void CUserInterface::UISetMIDIButtonChannel (unsigned uCh)
{
	// Mirrors the logic in Performance Config for handling MIDI channel configuration
	if (uCh == 0)
	{
		m_nMIDIButtonCh = CMIDIDevice::Disabled;
		LOGNOTE("MIDI Button channel not set");
	}
	else if (uCh <= CMIDIDevice::Channels)
	{
		m_nMIDIButtonCh = uCh - 1;
		LOGNOTE("MIDI Button channel set to: %d", m_nMIDIButtonCh+1);
	}
	else
	{
		m_nMIDIButtonCh = CMIDIDevice::OmniMode;
		LOGNOTE("MIDI Button channel set to: OMNI");
	}
}
