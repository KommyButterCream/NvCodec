#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "EncodeBench.h"
#include "RoundTrip.h"
#include "SelfTest.h"

namespace
{
	void PrintUsage()
	{
		printf_s(
			"NvEncodeBench - NVENC encode pipeline benchmark and regression harness\n"
			"\n"
			"Usage:\n"
			"  NvEncodeBench [bench] [options]   measure throughput and latency (default)\n"
			"  NvEncodeBench selftest            run the encode/decode regression cases\n"
			"  NvEncodeBench roundtrip [options]  encode -> decode round trip\n"
			"\n"
			"Options:\n"
			"  --width N            frame width                       (default 1920)\n"
			"  --height N           frame height                      (default 1080)\n"
			"  --frames N           frames to feed                    (default 600)\n"
			"  --fps N              target feed rate, 0 = unthrottled (default 60)\n"
			"  --buffers N          encoder slots, power of two >= 2   (default 4)\n"
			"  --queue N            queue slots, power of two >= 2     (default 4)\n"
			"  --pool N             source texture pool size          (default 8)\n"
			"  --keyframe N         request a keyframe every N frames (default 0 = off)\n"
			"  --sync               use DoEncode on this thread instead of the queue pump\n"
			"  --callback-delay N   burn N microseconds of CPU inside the encoded-frame\n"
			"                       callback, simulating the broadcast work   (default 0)\n"
			"  --bitrate N          target bitrate in bps            (default 5000000)\n"
			"  --vbv N              VBV size in bits, 0 = one frame  (default 0)\n"
			"  --latency-mode N     0 ultra-low, 1 low, 2 quality    (default 0)\n"
			"  --profile N          0 baseline, 1 main, 2 high       (default 2)\n"
			"  --intra-refresh N    1 on, 0 off (periodic IDR)       (default 1)\n"
			"  --reconfigure-at N        change the bitrate at frame N\n"
			"  --reconfigure-bitrate N   ...to this many bps\n"
			"  --contend-hz N       fake render thread takes the gate N times/sec (default 0)\n"
			"  --contend-us N       ...and holds it for N microseconds        (default 0)\n"
			"  --fault-at N         inject output failures at frame N (default 0)\n"
			"  --fault-count N      number of output failures to inject (default 0)\n"
			"  --csv PATH           write per-frame latency to a CSV file\n"
			"  --help               show this message\n"
			"\n"
			"Round trip options (NvEncodeBench roundtrip ...):\n"
			"  --width/--height/--frames/--fps/--buffers/--bitrate   as above\n"
			"  --dec-slots N        decoder output slots, power of two >= 2 (default 8)\n"
			"  --hold N             hold N decoded frames before releasing  (default 1)\n"
			"  --leak               never release decoded frames (pool exhaustion)\n"
			"  --contend-hz N       fake render thread takes the gate N times/sec\n"
			"  --contend-us N       ...and holds it for N microseconds\n"
			"\n"
			"Examples:\n"
			"  NvEncodeBench --width 1920 --height 1080 --fps 60 --frames 600\n"
			"  NvEncodeBench --fps 0 --frames 1000            (encoder ceiling)\n"
			"  NvEncodeBench --sync --fps 0 --frames 300      (latency floor, no thread hop)\n"
			"  NvEncodeBench --csv before.csv                 (baseline for A/B comparison)\n"
			"  NvEncodeBench selftest\n"
			"  NvEncodeBench roundtrip --frames 300              (encode -> decode loop)\n"
			"  NvEncodeBench roundtrip --hold 12 --dec-slots 8   (pool exhaustion)\n");
	}

	bool ParseUInt32(const char* text, uint32_t& value)
	{
		if (!text || *text == '\0')
			return false;

		char* end = nullptr;
		const unsigned long parsed = ::strtoul(text, &end, 10);
		if (!end || *end != '\0')
			return false;

		value = static_cast<uint32_t>(parsed);
		return true;
	}

	// 인자가 하나 더 필요한 옵션을 읽는다. 실패하면 false.
	bool TakeUInt32Argument(int argc, char** argv, int& index, const char* option, uint32_t& value)
	{
		if (index + 1 >= argc)
		{
			printf_s("[ARG ERROR] %s requires a value.\n", option);
			return false;
		}

		++index;
		if (!ParseUInt32(argv[index], value))
		{
			printf_s("[ARG ERROR] %s got a non-numeric value: %s\n", option, argv[index]);
			return false;
		}

		return true;
	}
}

int main(int argc, char** argv)
{
	Bench::BenchConfig config;
	config.frameCount = 600;

	int argIndex = 1;

	// 첫 인자가 옵션이 아니면 모드 이름으로 본다.
	if (argIndex < argc && argv[argIndex][0] != '-')
	{
		const char* mode = argv[argIndex];
		if (::_stricmp(mode, "selftest") == 0)
			return Bench::RunSelfTest();

		if (::_stricmp(mode, "roundtrip") == 0)
		{
			Bench::RoundTripConfig roundTripConfig;

			for (int i = argIndex + 1; i < argc; ++i)
			{
				const char* arg = argv[i];

				if (::_stricmp(arg, "--width") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.width)) return 2;
				}
				else if (::_stricmp(arg, "--height") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.height)) return 2;
				}
				else if (::_stricmp(arg, "--frames") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.frameCount)) return 2;
				}
				else if (::_stricmp(arg, "--fps") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.targetFps)) return 2;
				}
				else if (::_stricmp(arg, "--buffers") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.encodeBufferCount)) return 2;
				}
				else if (::_stricmp(arg, "--dec-slots") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.decodeSlotCount)) return 2;
				}
				else if (::_stricmp(arg, "--bitrate") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.bitrateBps)) return 2;
				}
				else if (::_stricmp(arg, "--hold") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.holdFrameCount)) return 2;
				}
				else if (::_stricmp(arg, "--leak") == 0)
				{
					roundTripConfig.leakFrames = true;
				}
				else if (::_stricmp(arg, "--contend-hz") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.contendHz)) return 2;
				}
				else if (::_stricmp(arg, "--contend-us") == 0)
				{
					if (!TakeUInt32Argument(argc, argv, i, arg, roundTripConfig.contendMicroseconds)) return 2;
				}
				else
				{
					printf_s("[ARG ERROR] Unknown roundtrip option: %s\n", arg);
					return 2;
				}
			}

			Bench::RoundTripBench roundTrip;
			if (!roundTrip.Setup(roundTripConfig.width, roundTripConfig.height))
			{
				printf_s("[FATAL] Round trip setup failed.\n");
				return 2;
			}

			Bench::RoundTripResult roundTripResult = {};
			if (!roundTrip.Run(roundTripConfig, roundTripResult))
				return 1;

			Bench::PrintRoundTripResult(roundTripConfig, roundTripResult);
			return roundTripResult.decoderFaulted ? 1 : 0;
		}

		if (::_stricmp(mode, "bench") != 0)
		{
			printf_s("[ARG ERROR] Unknown mode: %s\n\n", mode);
			PrintUsage();
			return 2;
		}

		++argIndex;
	}

	for (; argIndex < argc; ++argIndex)
	{
		const char* arg = argv[argIndex];

		if (::_stricmp(arg, "--help") == 0 || ::_stricmp(arg, "-h") == 0)
		{
			PrintUsage();
			return 0;
		}
		else if (::_stricmp(arg, "--sync") == 0)
		{
			config.asyncPipeline = false;
		}
		else if (::_stricmp(arg, "--csv") == 0)
		{
			if (argIndex + 1 >= argc)
			{
				printf_s("[ARG ERROR] --csv requires a path.\n");
				return 2;
			}
			config.csvPath = argv[++argIndex];
		}
		else if (::_stricmp(arg, "--width") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.width)) return 2;
		}
		else if (::_stricmp(arg, "--height") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.height)) return 2;
		}
		else if (::_stricmp(arg, "--frames") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.frameCount)) return 2;
		}
		else if (::_stricmp(arg, "--fps") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.targetFps)) return 2;
		}
		else if (::_stricmp(arg, "--buffers") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.encodeBufferCount)) return 2;
		}
		else if (::_stricmp(arg, "--queue") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.queueFrameCount)) return 2;
		}
		else if (::_stricmp(arg, "--pool") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.sourcePoolCount)) return 2;
		}
		else if (::_stricmp(arg, "--keyframe") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.keyFrameInterval)) return 2;
		}
		else if (::_stricmp(arg, "--callback-delay") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.callbackDelayMicroseconds)) return 2;
		}
		else if (::_stricmp(arg, "--bitrate") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.bitrateBps)) return 2;
		}
		else if (::_stricmp(arg, "--vbv") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.vbvBufferSizeBits)) return 2;
		}
		else if (::_stricmp(arg, "--intra-refresh") == 0)
		{
			uint32_t on = 1;
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, on)) return 2;
			config.enableIntraRefresh = (on != 0);
		}
		else if (::_stricmp(arg, "--latency-mode") == 0)
		{
			uint32_t mode = 0;
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, mode)) return 2;
			if (mode > 2) { printf_s("[ARG ERROR] --latency-mode must be 0..2\n"); return 2; }
			config.latencyMode = static_cast<NvEncLatencyMode>(mode);
		}
		else if (::_stricmp(arg, "--profile") == 0)
		{
			uint32_t p = 2;
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, p)) return 2;
			if (p > 2) { printf_s("[ARG ERROR] --profile must be 0..2\n"); return 2; }
			config.profile = static_cast<NvEncH264Profile>(p);
		}
		else if (::_stricmp(arg, "--reconfigure-at") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.reconfigureAtFrame)) return 2;
		}
		else if (::_stricmp(arg, "--reconfigure-bitrate") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.reconfigureBitrateBps)) return 2;
		}
		else if (::_stricmp(arg, "--contend-hz") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.contendHz)) return 2;
		}
		else if (::_stricmp(arg, "--contend-us") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.contendMicroseconds)) return 2;
		}
		else if (::_stricmp(arg, "--fault-at") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.faultInjectAfterFrames)) return 2;
		}
		else if (::_stricmp(arg, "--fault-count") == 0)
		{
			if (!TakeUInt32Argument(argc, argv, argIndex, arg, config.faultInjectCount)) return 2;
		}
		else
		{
			printf_s("[ARG ERROR] Unknown option: %s\n\n", arg);
			PrintUsage();
			return 2;
		}
	}

	if (config.frameCount == 0 || config.sourcePoolCount == 0)
	{
		printf_s("[ARG ERROR] --frames and --pool must be greater than zero.\n");
		return 2;
	}

	printf_s("NvEncodeBench  %ux%u  frames=%u  fps=%u  buffers=%u  queue=%u  pool=%u  %s\n",
		config.width, config.height, config.frameCount, config.targetFps,
		config.encodeBufferCount, config.queueFrameCount, config.sourcePoolCount,
		config.asyncPipeline ? "async" : "sync");

	Bench::EncodeBench bench;
	if (!bench.Setup(config.width, config.height, config.sourcePoolCount))
	{
		printf_s("[FATAL] Setup failed. A hardware D3D11 device with NVENC is required.\n");
		return 2;
	}

	Bench::BenchResult result = {};
	if (!bench.Run(config, result))
	{
		printf_s("[FATAL] Bench run failed.\n");
		return 1;
	}

	Bench::PrintResult(config, result);
	return result.faultedAtEnd ? 1 : 0;
}
