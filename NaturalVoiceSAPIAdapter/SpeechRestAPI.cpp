#include "pch.h"
#include "SpeechRestAPI.h"
#include "NetUtils.h"
#include "StrUtils.h"
#include "Mp3Decoder.h"
#include <format>

static std::string MakeRandomUuid()
{
	GUID guid;
	(void)CoCreateGuid(&guid);
	return std::format("{:08x}{:04x}{:04x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

static std::string GetTimeStamp()
{
	SYSTEMTIME tm;
	GetSystemTime(&tm);
	return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
		tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond, tm.wMilliseconds);
}

SpeechRestAPI::SpeechRestAPI()
{
	m_client.init_asio();
	m_client.set_tls_init_handler([](websocketpp::connection_hdl)
		{
			auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::sslv23_client);
			return ctx;
		});
	m_client.set_open_handler(std::bind_front(&SpeechRestAPI::OnOpen, this));
	m_client.set_message_handler(std::bind_front(&SpeechRestAPI::OnMessage, this));
	m_client.set_close_handler(std::bind_front(&SpeechRestAPI::OnClose, this));
	m_client.set_fail_handler(std::bind_front(&SpeechRestAPI::OnFail, this));

	// the two threads will run until this object is destroyed
	m_asioThread = std::thread(std::bind_front(&SpeechRestAPI::AsioThread, this));
	m_mp3Thread = std::thread(std::bind_front(&SpeechRestAPI::Mp3Thread, this));
}

SpeechRestAPI::~SpeechRestAPI()
{
	m_client.stop();
	m_isStopping = true;
	m_mp3ThreadNotifier.notify_all();
	m_asioThread.join();
	m_mp3Thread.join();
}

std::future<void> SpeechRestAPI::SpeakAsync(const std::wstring& ssml)
{
	std::error_code ec;
	auto con = m_client.get_connection(m_websocketUrl, ec);
	asio::detail::throw_error(ec);
	if (!m_key.empty())
		con->append_header("Ocp-Apim-Subscription-Key", m_key);
	std::string proxy = GetProxyForUrl(m_websocketUrl);
	if (!proxy.empty())
	{
		size_t schemeDelimPos = proxy.find("://");
		if (schemeDelimPos == proxy.npos)
		{
			con->set_proxy("http://" + proxy);
		}
		else
		{
			std::string_view scheme(proxy.data(), schemeDelimPos);
			if (EqualsIgnoreCase(scheme, "http"))
				con->set_proxy(proxy);
		}
	}

	m_ssml = ssml;
	m_lastWordPos = 0;
	m_lastSentencePos = 0;
	m_speakPromise = {};
	m_isPromiseSet.clear();
	auto fut = m_speakPromise.get_future();
	m_connection = std::move(con);
	m_client.connect(m_connection);

	return fut;
}

void SpeechRestAPI::Stop()
{
	std::error_code ec;
	if (m_connection)
	{
		m_connection->close(websocketpp::close::status::normal, {}, ec);
		m_connection.reset();
	}
	{
		std::lock_guard lock(m_mp3QueueMutex);
		m_mp3Queue = {}; // clear the unread data
		m_mp3QueueDone = true;
	}
	m_mp3ThreadNotifier.notify_one();
}

void SpeechRestAPI::SetSubscription(std::string key, const std::string& region)
{
	m_key = std::move(key);
	m_voiceListUrl = std::string("https://") + region + AZURE_TTS_HOST_AFTER_REGION + AZURE_VOICE_LIST_PATH;
	m_websocketUrl = std::string("wss://") + region + AZURE_TTS_HOST_AFTER_REGION + AZURE_WEBSOCKET_PATH;
}

void SpeechRestAPI::SetWebsocketUrl(std::string key, std::string websocketUrl)
{
	m_key = std::move(key);
	m_websocketUrl = std::move(websocketUrl);
}

bool SpeechRestAPI::IsCurrentConnection(const websocketpp::connection_hdl& hdl)
{
	std::error_code ec;
	return m_connection && m_connection == m_client.get_con_from_hdl(hdl, ec);
}

void SpeechRestAPI::AsioThread()
{
	m_client.start_perpetual();
	for (;;) // allows IO loop to recover from exceptions thrown from handlers
	{
		try
		{
			m_client.run();
			break; // exit the thread on normal return
		}
		catch (...)
		{
			// pass the exception, then rejoin the IO loop
			SpeakError(std::current_exception());
		}
	}
}

void SpeechRestAPI::Mp3Thread()
{
	std::optional<Mp3Decoder> mp3Decoder;
	mp3Decoder.emplace();

	for (;;)
	{
		try
		{
			std::string msg;

			{ // begin lock
				std::unique_lock lock(m_mp3QueueMutex);
				while (!m_isStopping && m_mp3Queue.empty())
				{
					if (m_mp3QueueDone)
					{
						SpeakComplete();
						mp3Decoder.emplace(); // re-create the decoder
						m_mp3QueueDone = false;
					}
					m_mp3ThreadNotifier.wait(lock);
				}
				if (m_isStopping)
					return;
				msg = std::move(m_mp3Queue.front());
				m_mp3Queue.pop();
			} // end lock

			// msg is the whole message (including header) from server
			// after "Path:audio\r\n" are audio binary data
			// Note that the first 2 bytes are not part of the header string
			std::string_view mp3data = msg;
			mp3data.remove_prefix(2);
			size_t delimPos = mp3data.find("Path:audio\r\n");
			if (delimPos == mp3data.npos) continue;
			mp3data.remove_prefix(delimPos + 12);

			// Sending audio data to SAPI can block, so do this without lock
			mp3Decoder->Convert(reinterpret_cast<const BYTE*>(mp3data.data()), (DWORD)mp3data.size(), AudioReceivedCallback);
		}
		catch (...)
		{
			SpeakError(std::current_exception());
		}
	}
}

void SpeechRestAPI::Mp3QueuePush(std::string&& msg)
{
	{
		std::lock_guard lock(m_mp3QueueMutex);
		m_mp3Queue.emplace(std::move(msg));
	}
	m_mp3ThreadNotifier.notify_one();
}

void SpeechRestAPI::Mp3QueueDone()
{
	{
		std::lock_guard lock(m_mp3QueueMutex);
		m_mp3QueueDone = true;
	}
	m_mp3ThreadNotifier.notify_one();
}

void SpeechRestAPI::SpeakComplete()
{
	if (!m_isPromiseSet.test_and_set())
		m_speakPromise.set_value();
}

void SpeechRestAPI::SpeakError(std::exception_ptr ex)
{
	if (!m_isPromiseSet.test_and_set())
		m_speakPromise.set_exception(ex);
}

void SpeechRestAPI::OnClose(websocketpp::connection_hdl hdl)
{
	if (!IsCurrentConnection(hdl))
		return;

	m_connection.reset();
	Mp3QueueDone();
}

void SpeechRestAPI::OnFail(websocketpp::connection_hdl hdl)
{
	if (!IsCurrentConnection(hdl))
		return;

	m_connection.reset();
	Mp3QueueDone();

	try
	{
		auto ec = m_client.get_con_from_hdl(hdl)->get_ec();
		asio::detail::throw_error(ec); // throw only if ec is an error
	}
	catch (...)
	{
		SpeakError(std::current_exception());
	}
}

// Send configuration and wait for audio data response
void SpeechRestAPI::OnOpen(websocketpp::connection_hdl hdl)
{
	if (!IsCurrentConnection(hdl))
		return;

	nlohmann::json json = {
		{"context", {
			{"synthesis", {
				{"audio", {
					{"metadataOptions", {
						{"bookmarkEnabled", (bool)BookmarkCallback},
						{"punctuationBoundaryEnabled", (bool)PunctuationBoundaryCallback},
						{"sentenceBoundaryEnabled", (bool)SentenceBoundaryCallback},
						{"wordBoundaryEnabled", (bool)WordBoundaryCallback},
						{"visemeEnabled", (bool)VisemeCallback},
					}},
					{"outputFormat", "audio-24khz-96kbitrate-mono-mp3"}
				}},
				{"language", {
					{"autoDetection", false}
				}}
			}}
		}}
	};

	std::string reqId = MakeRandomUuid();

	m_client.send(hdl,
		"X-Timestamp:" + GetTimeStamp() + "\r\n"
		"Content-Type:application/json; charset=utf-8\r\n"
		"Path:speech.config\r\n\r\n"
		+ json.dump(),
		websocketpp::frame::opcode::text);

	m_client.send(hdl,
		"X-Timestamp:" + GetTimeStamp() + "\r\n"
		"X-RequestId:" + reqId + "\r\n"
		"Content-Type:application/ssml+xml\r\n"
		"Path:ssml\r\n\r\n"
		+ WStringToUTF8(m_ssml),
		websocketpp::frame::opcode::text);
}

void SpeechRestAPI::OnMessage(websocketpp::connection_hdl hdl, WSClient::message_ptr msg)
{
	if (!IsCurrentConnection(hdl))
		return;

	if (msg->get_opcode() == websocketpp::frame::opcode::binary)
	{
		// If the message is binary, place this message in the queue to let the MP3 thread process it
		Mp3QueuePush(std::move(msg->get_raw_payload()));
	}
	else
	{
		// If not, after "Path:xxx\r\n\r\n" are JSON texts
		std::string_view text = msg->get_payload();
		size_t pathStartPos = text.find("Path:");
		if (pathStartPos == text.npos) return;
		pathStartPos += 5;
		size_t pathEndPos = text.find("\r\n\r\n", pathStartPos);
		if (pathEndPos == text.npos) return;

		std::string_view path = text.substr(pathStartPos, pathEndPos - pathStartPos);
		if (path == "audio.metadata")
		{
			const auto json = nlohmann::json::parse(text.substr(pathEndPos + 4));
			for (auto& event : json.at("Metadata"))
				OnSynthEvent(event);
		}
		else if (path == "turn.end")
		{
			// Data receiving completed
			m_connection->close(websocketpp::close::status::normal, {});
			m_connection.reset();
			Mp3QueueDone();
		}
	}
}

void SpeechRestAPI::OnSynthEvent(const nlohmann::json& metadata)
{
	std::string type = metadata.at("Type");
	auto& data = metadata.at("Data");
	uint64_t offset = data.at("Offset");

	if (type == "Viseme")
	{
		if (VisemeCallback)
			VisemeCallback(offset, data.at("VisemeId"));
	}
	else if (type == "WordBoundary")
	{
		auto& info = data.at("text");
		if (info.at("BoundaryType").get<std::string>() == "PunctuationBoundary")
		{
			if (PunctuationBoundaryCallback)
				PunctuationBoundaryCallback(offset, (uint32_t)FindWord(info.at("Text"), m_lastWordPos), info.at("Length"));
		}
		else
		{
			if (WordBoundaryCallback)
				WordBoundaryCallback(offset, (uint32_t)FindWord(info.at("Text"), m_lastWordPos), info.at("Length"));
		}
	}
	else if (type == "SentenceBoundary")
	{
		auto& info = data.at("text");
		if (SentenceBoundaryCallback)
			SentenceBoundaryCallback(offset, (uint32_t)FindWord(info.at("Text"), m_lastSentencePos), info.at("Length"));
	}
	else if (type == "SessionEnd")
	{
		if (SessionEndCallback)
			SessionEndCallback(offset);
	}
	else if (type == "Bookmark")
	{
		if (BookmarkCallback)
			BookmarkCallback(offset, data.at("Bookmark"));
	}
}

static std::wstring XmlEscape(const std::wstring& str)
{
	std::wstring ret;
	ret.reserve(str.size());
	LPCWSTR pEnd = str.c_str() + str.size();

	for (LPCWSTR pCh = str.c_str(); pCh != pEnd; pCh++)
	{
		switch (*pCh)
		{
		case '<': ret.append(L"&lt;"); break;
		case '>': ret.append(L"&gt;"); break;
		case '&': ret.append(L"&amp;"); break;
		case '"': ret.append(L"&quot;"); break;
		case '\'': ret.append(L"&apos;"); break;
		default: ret.push_back(*pCh); break;
		}
	}

	return ret;
}

// Speech API only returns the word text and word length in a WordBoundary event,
// so we have to calculate the text offset of the word ourselves.
// Returned offset is in WCHARs.
// Argument lastPos [in, out] is the variable that records the last boundary position.
size_t SpeechRestAPI::FindWord(const std::string& utf8Word, size_t& lastPos)
{
	// Escape XML chars, otherwise words such as "you're" will not be matched
	std::wstring word = XmlEscape(UTF8ToWString(utf8Word));
	std::wstring_view ssml = m_ssml;
	size_t startpos = lastPos;
	size_t wordPos;
	while ((wordPos = ssml.find(word, startpos)) != ssml.npos) // look for an occurrence of the word
	{
		// check if there's unmatched "<>" pair before this word
		std::wstring_view beforeWord = ssml.substr(startpos, wordPos - startpos);
		for (;;)
		{
			// look for a '<'
			size_t tagStart = beforeWord.find('<');
			if (tagStart == beforeWord.npos) // no more '<', meaning all "<>" matched or there's no "<>"
			{
				lastPos = wordPos + word.size();
				return wordPos; // exit here
			}

			// look for the matching '>'
			size_t tagEnd = beforeWord.find('>', tagStart + 1);
			if (tagEnd == beforeWord.npos) // no matching '>', so the word is inside a "<>" pair
			{
				break;
			}
			beforeWord.remove_prefix(tagEnd + 1); // look for the next "<>" pair
		}

		// Now we confirmed that the word is inside a "<>" pair
		// Skip to the next '>' and continue searching
		startpos = ssml.find('>', wordPos + word.size());
	}
	return ssml.npos;
}