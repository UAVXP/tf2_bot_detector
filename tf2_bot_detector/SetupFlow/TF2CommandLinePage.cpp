#include "TF2CommandLinePage.h"
#include "Config/Settings.h"
#include "ImGui_TF2BotDetector.h"
#include "Log.h"
#include "PlatformSpecific/Processes.h"
#include "PlatformSpecific/Shell.h"

#include <srcon/srcon.h>
#include <mh/text/string_insertion.hpp>

#include <chrono>
#include <random>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace tf2_bot_detector;

#ifdef _DEBUG
namespace tf2_bot_detector
{
	extern uint32_t g_StaticRandomSeed;
}
#endif

static std::string GenerateRandomRCONPassword(size_t length = 16)
{
	std::mt19937 generator;
#ifdef _DEBUG
	if (g_StaticRandomSeed != 0)
	{
		generator.seed(g_StaticRandomSeed);
	}
	else
#endif
	{
		std::random_device randomSeed;
		generator.seed(randomSeed());
	}

	constexpr char PALETTE[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	std::uniform_int_distribution<size_t> dist(0, std::size(PALETTE) - 1);

	std::string retVal(length, '\0');
	for (size_t i = 0; i < length; i++)
		retVal[i] = PALETTE[dist(generator)];

	return retVal;
}

static uint16_t GenerateRandomRCONPort()
{
	std::mt19937 generator;
#ifdef _DEBUG
	if (g_StaticRandomSeed != 0)
	{
		generator.seed(g_StaticRandomSeed + 314);
	}
	else
#endif
	{
		std::random_device randomSeed;
		generator.seed(randomSeed());
	}

	// Some routers have issues handling high port numbers. By restricting
	// ourselves to these high port numbers, we add another layer of security.
	std::uniform_int_distribution<uint16_t> dist(40000, 65535);
	return dist(generator);
}

void TF2CommandLinePage::Data::TryUpdateCmdlineArgs()
{
	if (m_CommandLineArgsFuture.valid() &&
		m_CommandLineArgsFuture.wait_for(0s) == std::future_status::ready)
	{
		m_CommandLineArgs = m_CommandLineArgsFuture.get();
		m_CommandLineArgsFuture = {};
		m_Ready = true;
	}

	if (!m_CommandLineArgsFuture.valid())
	{
		// See about starting a new update

		const auto curTime = clock_t::now();
		if (!m_Ready || (curTime >= (m_LastCLUpdate + CL_UPDATE_INTERVAL)))
		{
			m_CommandLineArgsFuture = Processes::GetTF2CommandLineArgsAsync();
			m_LastCLUpdate = curTime;
		}
	}
}

bool TF2CommandLinePage::Data::HasUseRconCmdLineFlag() const
{
	if (m_CommandLineArgs.size() != 1)
		return false;

	return m_CommandLineArgs.at(0).find("-usercon") != std::string::npos;
}

bool TF2CommandLinePage::ValidateSettings(const Settings& settings) const
{
	if (!Processes::IsTF2Running())
		return false;
	if (!m_Data.HasUseRconCmdLineFlag())
		return false;

	return true;
}

static void OpenTF2(const std::string_view& rconPassword, uint16_t rconPort)
{
	std::string url;
	url << "steam://run/440//"
		" -usercon"
		" +ip 0.0.0.0 +alias ip"
		" +sv_rcon_whitelist_address 127.0.0.1 +alias sv_rcon_whitelist_address"
		" +rcon_password " << rconPassword <<
		" +hostport " << rconPort << " +alias hostport"
		" +con_timestamp 1 +alias con_timestamp"
		" +net_start"
		" -condebug"
		" -conclearlog";

	Shell::OpenURL(std::move(url));
}

TF2CommandLinePage::RCONClientData::RCONClientData(std::string pwd, uint16_t port) :
	m_Client(std::make_unique<srcon::async_client>())
{
	srcon::srcon_addr addr;
	addr.addr = "127.0.0.1";
	addr.pass = std::move(pwd);
	addr.port = port;

	m_Client->set_addr(std::move(addr));
}

bool TF2CommandLinePage::RCONClientData::Update()
{
	if (m_Success)
		return true;

	if (m_Future.valid() && m_Future.wait_for(0s) == std::future_status::ready)
	{
		try
		{
			auto response = m_Future.get();
			ImGui::TextColoredUnformatted({ 0, 1, 0, 1 }, response);
			m_Success = true;
		}
		catch (const srcon::srcon_error& e)
		{
			DebugLog(std::string(__FUNCTION__) << "(): " << e.what());

			using srcon::srcon_errc;
			switch (e.get_errc())
			{
			case srcon_errc::bad_rcon_password:
				m_MessageColor = { 1, 0, 0, 1 };
				m_Message = "Bad rcon password, this should never happen!";
				break;
			case srcon_errc::rcon_connect_failed:
				m_MessageColor = { 1, 1, 0.5, 1 };
				m_Message = "Retrying RCON connection...";
				break;
			case srcon_errc::socket_send_failed:
				m_MessageColor = { 1, 1, 0.5, 1 };
				m_Message = "TF2 not yet accepting RCON commands...";
				break;
			default:
				m_MessageColor = { 1, 1, 0, 1 };
				m_Message = "Unexpected error: "s << e.what();
				break;
			}
			m_Future = {};
		}
		catch (const std::exception& e)
		{
			DebugLogWarning(std::string(__FUNCTION__) << "(): " << e.what());
			m_MessageColor = { 1, 0, 0, 1 };
			m_Message = "RCON connection unsuccessful: "s << e.what();
			m_Future = {};
		}
	}

	if (!m_Future.valid())
		m_Future = m_Client->send_command_async("echo RCON connection successful.", false);

	ImGui::TextColoredUnformatted(m_MessageColor, m_Message);

	return m_Success;
}

auto TF2CommandLinePage::OnDraw(const DrawState& ds) -> OnDrawResult
{
	m_Data.TryUpdateCmdlineArgs();

	const auto LaunchTF2Button = [&]
	{
		ImGui::NewLine();
		ImGui::EnabledSwitch(m_Data.m_Ready, [&]
			{
				if (ImGui::Button("Launch TF2"))
					OpenTF2(m_Data.m_RCONPassword, m_Data.m_RCONPort);

			}, "Finding command line arguments...");
	};

	if (m_Data.m_CommandLineArgs.empty())
	{
		ImGui::TextUnformatted("Waiting for TF2 to be opened...");
		LaunchTF2Button();
	}
	else if (m_Data.m_CommandLineArgs.size() > 1)
	{
		ImGui::TextUnformatted("More than one instance of hl2.exe found. Please close the other instances.");

		ImGui::EnabledSwitch(false, LaunchTF2Button, "TF2 is currently running. Please close it first.");
	}
	else if (!m_Data.HasUseRconCmdLineFlag())
	{
		ImGui::TextUnformatted("TF2 must be run with the -usercon command line flag. You can either add that flag under Launch Options in Steam, or close TF2 and open it with the button below.");

		ImGui::EnabledSwitch(false, LaunchTF2Button, "TF2 is currently running. Please close it first.");
	}
	else if (!m_Data.m_RCONSuccess)
	{
		ImGui::TextUnformatted("Connecting to TF2 on 127.0.0.1:"s << m_Data.m_RCONPort
			<< " with password " << m_Data.m_RCONPassword << "...");

		if (!m_Data.m_TestRCONClient)
			m_Data.m_TestRCONClient.emplace(m_Data.m_RCONPassword, m_Data.m_RCONPort);

		m_Data.m_RCONSuccess = m_Data.m_TestRCONClient->Update();
		if (m_Data.m_RCONSuccess)
			m_Data.m_TestRCONClient.reset();
	}
	else
	{
		return OnDrawResult::EndDrawing;
	}

	return OnDrawResult::ContinueDrawing;
}

void TF2CommandLinePage::Init(const Settings& settings)
{
	m_Data = {};
	m_Data.m_RCONPassword = GenerateRandomRCONPassword();
	m_Data.m_RCONPort = GenerateRandomRCONPort();
}

void TF2CommandLinePage::Commit(Settings& settings)
{
	settings.m_Unsaved.m_RCONPassword = m_Data.m_RCONPassword;
	settings.m_Unsaved.m_RCONPort = m_Data.m_RCONPort;
}