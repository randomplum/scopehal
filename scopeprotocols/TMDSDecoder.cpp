/***********************************************************************************************************************
*                                                                                                                      *
* ANTIKERNEL v0.1                                                                                                      *
*                                                                                                                      *
* Copyright (c) 2012-2020 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of TMDSDecoder
 */

#include "../scopehal/scopehal.h"
#include "../scopehal/ChannelRenderer.h"
#include "../scopehal/TextRenderer.h"
#include "TMDSRenderer.h"
#include "TMDSDecoder.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

TMDSDecoder::TMDSDecoder(string color)
	: ProtocolDecoder(OscilloscopeChannel::CHANNEL_TYPE_COMPLEX, color, CAT_SERIAL)
{
	//Set up channels
	m_signalNames.push_back("data");
	m_channels.push_back(NULL);

	m_signalNames.push_back("clk");
	m_channels.push_back(NULL);

	m_lanename = "Lane number";
	m_parameters[m_lanename] = ProtocolDecoderParameter(ProtocolDecoderParameter::TYPE_INT);
	m_parameters[m_lanename].SetIntVal(0);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Factory methods

bool TMDSDecoder::NeedsConfig()
{
	//baud rate has to be set
	return true;
}

ChannelRenderer* TMDSDecoder::CreateRenderer()
{
	return new TMDSRenderer(this);
}

bool TMDSDecoder::ValidateChannel(size_t i, OscilloscopeChannel* channel)
{
	if( (i == 0) && (channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL) && (channel->GetWidth() == 1) )
		return true;
	if( (i == 1) && (channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL) && (channel->GetWidth() == 1) )
		return true;
	return false;
}

string TMDSDecoder::GetProtocolName()
{
	return "8b/10b (TMDS)";
}

void TMDSDecoder::SetDefaultName()
{
	char hwname[256];
	snprintf(hwname, sizeof(hwname), "TMDS(%s)", m_channels[0]->m_displayname.c_str());
	m_hwname = hwname;
	m_displayname = m_hwname;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void TMDSDecoder::Refresh()
{
	//Get the input data
	if( (m_channels[0] == NULL) || (m_channels[1] == NULL) )
	{
		SetData(NULL);
		return;
	}
	DigitalCapture* din = dynamic_cast<DigitalCapture*>(m_channels[0]->GetData());
	DigitalCapture* clkin = dynamic_cast<DigitalCapture*>(m_channels[1]->GetData());
	if( (din == NULL) || (clkin == NULL) )
	{
		SetData(NULL);
		return;
	}

	//Create the capture
	TMDSCapture* cap = new TMDSCapture;
	cap->m_timescale = 1;
	cap->m_startTimestamp = din->m_startTimestamp;
	cap->m_startPicoseconds = din->m_startPicoseconds;

	//Record the value of the data stream at each clock edge
	vector<DigitalSample> sampdata;
	SampleOnAnyEdges(din, clkin, sampdata);

	/*
		Look for preamble data. We need this to synchronize. (HDMI 1.4 spec section 5.4.2)

		TMDS sends the LSB first.
		Since element 0 of a C++ array is leftmost, this table has bit ordering mirrored from the spec.
	 */
	static const bool control_codes[4][10] =
	{
		{ 0, 0, 1, 0, 1, 0, 1, 0, 1, 1 },
		{ 1, 1, 0, 1, 0, 1, 0, 1, 0, 0 },
		{ 0, 0, 1, 0, 1, 0, 1, 0, 1, 0 },
		{ 1, 1, 0, 1, 0, 1, 0, 1, 0, 1 }
	};

	size_t max_preambles = 0;
	size_t max_offset = 0;
	for(size_t offset=0; offset < 10; offset ++)
	{
		size_t num_preambles[4] = {0};
		for(size_t i=0; i<sampdata.size() - 20; i += 10)
		{
			//Look for control code "j" at phase "offset", position "i" within the data stream
			for(size_t j=0; j<4; j++)
			{
				bool match = true;
				for(size_t k=0; k<10; k++)
				{
					if(sampdata[i+offset+k].m_sample != control_codes[j][k])
						match = false;
				}
				if(match)
				{
					num_preambles[j] ++;
					break;
				}
			}
		}

		for(size_t j=0; j<4; j++)
		{
			if(num_preambles[j] > max_preambles)
			{
				max_preambles = num_preambles[j];
				max_offset = offset;
			}
		}
	}

	int lane = m_parameters[m_lanename].GetIntVal();

	//HDMI Video guard band (HDMI 1.4 spec 5.2.2.1)
	static const bool video_guard[3][10] =
	{
		{ 0, 0, 1, 1, 0, 0, 1, 1, 0, 1 },
		{ 1, 1, 0, 0, 1, 1, 0, 0, 1, 0 },		//also used for data guard band, 5.2.3.3
		{ 0, 0, 1, 1, 0, 0, 1, 1, 0, 1 },
	};

	//TODO: TERC4 (5.4.3)

	enum
	{
		TYPE_DATA,
		TYPE_PREAMBLE,
		TYPE_GUARD
	} last_symbol_type = TYPE_DATA;

	//Decode the actual data
	for(size_t i=max_offset; i<sampdata.size()-11; i+= 10)
	{
		bool match = true;

		//Check for control codes at any point in the sequence
		for(size_t j=0; j<4; j++)
		{
			match = true;
			for(size_t k=0; k<10; k++)
			{
				if(sampdata[i+k].m_sample != control_codes[j][k])
					match = false;
			}

			if(match)
			{
				cap->m_samples.push_back(TMDSSample(
					sampdata[i].m_offset,
					sampdata[i+10].m_offset - sampdata[i].m_offset,
					TMDSSymbol(TMDSSymbol::TMDS_TYPE_CONTROL, j)));
				last_symbol_type = TYPE_PREAMBLE;
				break;
			}
		}

		if(match)
			continue;

		//Check for HDMI video/control leading guard band
		if( (last_symbol_type == TYPE_PREAMBLE) || (last_symbol_type == TYPE_GUARD) )
		{
			match = true;
			for(size_t k=0; k<10; k++)
			{
				if(sampdata[i+k].m_sample != video_guard[lane][k])
					match = false;
			}

			if(match)
			{
				cap->m_samples.push_back(TMDSSample(
					sampdata[i].m_offset,
					sampdata[i+10].m_offset - sampdata[i].m_offset,
					TMDSSymbol(TMDSSymbol::TMDS_TYPE_GUARD, 0)));
				last_symbol_type = TYPE_GUARD;
				break;
			}
		}

		if(match)
			continue;

		//Whatever is left is assumed to be video data
		bool d9 = sampdata[i+9].m_sample;
		bool d8 = sampdata[i+8].m_sample;

		uint8_t d = sampdata[i+0].m_sample |
					(sampdata[i+1].m_sample << 1) |
					(sampdata[i+2].m_sample << 2) |
					(sampdata[i+3].m_sample << 3) |
					(sampdata[i+4].m_sample << 4) |
					(sampdata[i+5].m_sample << 5) |
					(sampdata[i+6].m_sample << 6) |
					(sampdata[i+7].m_sample << 7);

		if(d9)
			d ^= 0xff;

		if(d8)
			d ^= (d << 1);
		else
			d ^= (d << 1) ^ 0xfe;

		cap->m_samples.push_back(TMDSSample(
			sampdata[i].m_offset,
			sampdata[i+10].m_offset - sampdata[i].m_offset,
			TMDSSymbol(TMDSSymbol::TMDS_TYPE_DATA, d)));
		last_symbol_type = TYPE_DATA;
	}

	SetData(cap);
}
