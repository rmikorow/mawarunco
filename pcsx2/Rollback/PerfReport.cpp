// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Rollback/PerfReport.h"
#include "Rollback/BootShortcut.h"
#include "Rollback/Netplay.h"
#include "Rollback/RunAhead.h"

#include "BuildVersion.h"
#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <psapi.h>
#endif

namespace PerfReport
{
	static constexpr double INTERVAL_SECONDS = 10.0;
	static constexpr double STUTTER_MS = 25.0; // a frame interval this long is visible (normal: 16.7 ms)
	static constexpr size_t KEEP_REPORTS = 10;

	struct Interval
	{
		u32 frames = 0;
		double work_sum = 0.0;
		double work_max = 0.0;
		u32 stutters = 0;
		double interval_max = 0.0;
		u32 saves = 0;
		double save_sum = 0.0;
		double save_max = 0.0;
		u32 loads = 0;
		double load_sum = 0.0;
		double load_max = 0.0;
	};

	struct Sample
	{
		double t;        // seconds since the report started
		double mem_mb;   // private memory of the process
		double work_avg; // ms
	};

	static bool s_enabled_checked = false;
	static bool s_enabled = false;
	static std::FILE* s_file = nullptr;
	static Common::Timer s_session;   // since the report started
	static Common::Timer s_interval;  // since the last report line
	static Common::Timer s_frame;     // since the end of the previous frame
	static bool s_have_frame = false; // s_frame is valid
	static Interval s_cur;
	static std::vector<Sample> s_samples;
	static u64 s_total_frames = 0;
	static u64 s_total_stutters = 0;
	static double s_total_work = 0.0;
	static double s_worst_work = 0.0;
	static double s_mem_peak = 0.0;
	static u64 s_prev_rollbacks = 0;
	static bool s_netplay_seen = false;
	static bool s_desync_seen = false;

	static bool Enabled();
	static void Open();
	static void Write(const std::string& line);
	static void WriteInterval();
	static double ProcessMemoryMB();
	static std::string SystemDescription();
	static void RemoveOldReports();
} // namespace PerfReport

bool PerfReport::Enabled()
{
	if (!s_enabled_checked)
	{
		s_enabled_checked = true;
		s_enabled = Host::GetBaseBoolSettingValue("EmuCore/Rollback", "PerfReport", true);
	}
	return s_enabled;
}

double PerfReport::ProcessMemoryMB()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX pmc = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
		return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
#endif
	return 0.0;
}

std::string PerfReport::SystemDescription()
{
	// Hardware and settings only: no paths, addresses or names.
	const CPUInfo& cpu = GetCPUInfo();
	std::string ram;
#ifdef _WIN32
	MEMORYSTATUSEX ms = {};
	ms.dwLength = sizeof(ms);
	if (GlobalMemoryStatusEx(&ms))
		ram = fmt::format("{:.0f} GB", static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
#endif
	std::string gpu = g_gs_device ? std::string(g_gs_device->GetName()) : std::string("unknown");
	return fmt::format("Mawarunco {} ({})\nOS: {}\nCPU: {} ({} threads)\nRAM: {}\nGPU: {}\n"
					   "Renderer: {}, upscale {}x, vsync {}\nRun-ahead: {} frame(s)\n",
		BuildVersion::GitRev, std::string_view(BuildVersion::GitHash).substr(0, 9), GetOSVersionString(), cpu.name,
		cpu.num_threads, ram.empty() ? "unknown" : ram, gpu, Pcsx2Config::GSOptions::GetRendererName(EmuConfig.GS.Renderer),
		EmuConfig.GS.UpscaleMultiplier, EmuConfig.GS.VsyncEnable ? "on" : "off", RunAhead::Frames());
}

void PerfReport::RemoveOldReports()
{
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(EmuFolders::Logs.c_str(), "perf-*.txt", FILESYSTEM_FIND_FILES, &files);
	if (files.size() < KEEP_REPORTS)
		return;
	// Names sort by date and time.
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.FileName < b.FileName; });
	for (size_t i = 0; i + KEEP_REPORTS <= files.size(); i++)
		FileSystem::DeleteFilePath(files[i].FileName.c_str());
}

void PerfReport::Open()
{
	RemoveOldReports();
	const std::time_t now = std::time(nullptr);
	std::tm tm = {};
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	const std::string path = Path::Combine(EmuFolders::Logs, fmt::format("perf-{}.txt", stamp));
	s_file = FileSystem::OpenCFile(path.c_str(), "wb");
	if (!s_file)
	{
		Console.Warning("PerfReport: cannot create the report file.");
		s_enabled = false;
		return;
	}
	char started[32];
	std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M", &tm);
	const bool netplay = Netplay::IsActive();
	Write(fmt::format("MAWARUNCO PERFORMANCE REPORT\nStarted: {}\nMode: {}\n{}", started,
		netplay ? "online" : (BootShortcut::MemoryCardsOff() ? "training" : "offline"), SystemDescription()));
	Write("Columns: time | frames | frame work avg/max ms (16.7 = full frame) | stutters (frames over 25 ms), "
		  "worst ms | memory MB (change) | snapshot save avg/max ms, restore avg/max ms | rollbacks, ping\n");
	s_session.Reset();
	s_interval.Reset();
	s_cur = {};
	s_samples.clear();
	s_total_frames = s_total_stutters = 0;
	s_total_work = s_worst_work = 0.0;
	s_mem_peak = 0.0;
	s_prev_rollbacks = 0;
	s_netplay_seen = s_desync_seen = false;
}

void PerfReport::Write(const std::string& line)
{
	if (!s_file)
		return;
	std::fwrite(line.data(), 1, line.size(), s_file);
	std::fflush(s_file);
}

void PerfReport::OnFrameWorkDone()
{
	// Not while the boot shortcut runs the game unthrottled to the menu: that is not play.
	if (!Enabled() || BootShortcut::IsActive())
	{
		s_have_frame = false;
		return;
	}
	if (!s_file)
	{
		Open();
		if (!s_file)
			return;
	}
	if (s_have_frame)
	{
		const double work = s_frame.GetTimeMilliseconds();
		s_cur.work_sum += work;
		s_cur.work_max = std::max(s_cur.work_max, work);
	}
}

void PerfReport::OnFrameEnd()
{
	if (!s_file || BootShortcut::IsActive())
		return;
	if (s_have_frame)
	{
		const double interval = s_frame.GetTimeMilliseconds();
		s_cur.frames++;
		if (interval > STUTTER_MS)
			s_cur.stutters++;
		s_cur.interval_max = std::max(s_cur.interval_max, interval);
	}
	s_frame.Reset();
	s_have_frame = true;
	if (s_interval.GetTimeSeconds() >= INTERVAL_SECONDS)
		WriteInterval();
}

void PerfReport::OnSnapshot(bool load, double ms)
{
	if (!s_file)
		return;
	if (load)
	{
		s_cur.loads++;
		s_cur.load_sum += ms;
		s_cur.load_max = std::max(s_cur.load_max, ms);
	}
	else
	{
		s_cur.saves++;
		s_cur.save_sum += ms;
		s_cur.save_max = std::max(s_cur.save_max, ms);
	}
}

void PerfReport::WriteInterval()
{
	s_interval.Reset();
	const double t = s_session.GetTimeSeconds();
	const double mem = ProcessMemoryMB();
	const double prev_mem = s_samples.empty() ? mem : s_samples.back().mem_mb;
	const double work_avg = s_cur.frames ? s_cur.work_sum / s_cur.frames : 0.0;
	s_samples.push_back({t, mem, work_avg});
	s_total_frames += s_cur.frames;
	s_total_stutters += s_cur.stutters;
	s_total_work += s_cur.work_sum;
	s_worst_work = std::max(s_worst_work, s_cur.work_max);
	s_mem_peak = std::max(s_mem_peak, mem);

	std::string net;
	Netplay::PerfInfo info;
	if (Netplay::GetPerfInfo(&info))
	{
		s_netplay_seen = true;
		s_desync_seen |= info.desync;
		net = fmt::format(" | rollbacks {}, ping {} ms{}", info.rollbacks - s_prev_rollbacks, info.ping_ms,
			info.desync ? ", DESYNC" : "");
		s_prev_rollbacks = info.rollbacks;
	}
	auto avg = [](double sum, u32 n) { return n ? sum / n : 0.0; };
	const int minutes = static_cast<int>(t) / 60;
	const int seconds = static_cast<int>(t) % 60;
	Write(fmt::format("{:3}:{:02} | {:4} | {:5.2f} / {:5.2f} | {:3}, {:5.1f} | {:6.0f} ({:+5.0f}) | {:4.2f} / {:4.2f}, "
					  "{:4.2f} / {:4.2f}{}\n",
		minutes, seconds, s_cur.frames, work_avg, s_cur.work_max, s_cur.stutters, s_cur.interval_max, mem, mem - prev_mem,
		avg(s_cur.save_sum, s_cur.saves), s_cur.save_max, avg(s_cur.load_sum, s_cur.loads), s_cur.load_max, net));
	s_cur = {};
}

void PerfReport::OnShutdown()
{
	s_have_frame = false;
	if (!s_file)
		return;
	if (s_cur.frames > 0)
		WriteInterval();

	const double t = s_session.GetTimeSeconds();
	std::string summary = fmt::format("\nSUMMARY\nDuration: {}:{:02} min, {} frames\n", static_cast<int>(t) / 60,
		static_cast<int>(t) % 60, s_total_frames);
	std::vector<std::string> warnings;
	if (s_total_frames > 0)
	{
		const double work_avg = s_total_work / static_cast<double>(s_total_frames);
		const double stutter_per_min = s_total_stutters / std::max(t / 60.0, 1.0 / 60.0);
		summary += fmt::format("Frame work: average {:.2f} ms, worst {:.2f} ms (a frame has 16.7 ms)\n", work_avg, s_worst_work);
		summary += fmt::format("Stutters: {} ({:.1f} per minute)\n", s_total_stutters, stutter_per_min);
		if (work_avg > 14.0)
			warnings.push_back("The PC barely keeps up: on average more than 14 of 16.7 ms per frame are used.");
		if (stutter_per_min > 2.0)
			warnings.push_back(fmt::format("Frequent stutter: {:.1f} frames per minute took over 25 ms.", stutter_per_min));
	}

	// Trends, from the samples after the first minute (loading and caches settle there).
	std::vector<Sample> late;
	for (const Sample& s : s_samples)
	{
		if (s.t >= 60.0)
			late.push_back(s);
	}
	if (!s_samples.empty())
		summary += fmt::format("Memory: start {:.0f} MB, end {:.0f} MB, peak {:.0f} MB\n", s_samples.front().mem_mb,
			s_samples.back().mem_mb, s_mem_peak);
	if (late.size() >= 6)
	{
		// Least-squares slope of memory over time, in MB per minute.
		double st = 0, sm = 0, stt = 0, stm = 0;
		for (const Sample& s : late)
		{
			st += s.t;
			sm += s.mem_mb;
			stt += s.t * s.t;
			stm += s.t * s.mem_mb;
		}
		const double n = static_cast<double>(late.size());
		const double denom = n * stt - st * st;
		const double slope = (denom != 0.0) ? (n * stm - st * sm) / denom * 60.0 : 0.0;
		summary += fmt::format("Memory trend: {:+.1f} MB per minute\n", slope);
		if (slope > 20.0)
			warnings.push_back(fmt::format("Memory keeps growing ({:+.0f} MB per minute). Please report this.", slope));

		const size_t k = std::min<size_t>(6, late.size() / 2); // about one minute at each end
		double first = 0, last = 0;
		for (size_t i = 0; i < k; i++)
		{
			first += late[i].work_avg;
			last += late[late.size() - 1 - i].work_avg;
		}
		first /= static_cast<double>(k);
		last /= static_cast<double>(k);
		summary += fmt::format("Frame work trend: {:.2f} ms early, {:.2f} ms late\n", first, last);
		if (last > first * 1.2 && last - first > 1.5)
			warnings.push_back(fmt::format("Frames get slower over time ({:.1f} -> {:.1f} ms). Please report this.", first, last));
	}
	if (s_netplay_seen && s_desync_seen)
		warnings.push_back("A desync was detected. Please send this report and logs\\emulog.txt.");

	summary += warnings.empty() ? "Warnings: none\n" : "Warnings:\n";
	for (const std::string& w : warnings)
		summary += "  WARNING: " + w + "\n";
	Write(summary);
	std::fclose(s_file);
	s_file = nullptr;
}
