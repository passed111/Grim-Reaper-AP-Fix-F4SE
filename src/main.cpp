#include "pch.h"

#include <atomic>
#include <intrin.h>
#include <chrono>
#include <format>
#include <thread>
#include <functional>
#include <vector>
#include <cstring>

namespace grim
{
	std::atomic<bool> g_killPending{ false };
	std::uintptr_t    g_killRetAddr{ 0 };

	using HandleEntryPointFn = bool (*)(RE::BGSEntryPoint::ENTRY_POINT, RE::Actor*, void*, void*, void*);
	HandleEntryPointFn g_origHandleEntryPoint{ nullptr };

	bool HookedHandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner,
		void* a_p3, void* a_p4, void* a_p5)
	{
		if (!g_origHandleEntryPoint) {
			return false;
		}

		if (a_ep != RE::BGSEntryPoint::ENTRY_POINT::kModVATSPlayerAPKillAwardChance || !a_owner) {
			return g_origHandleEntryPoint(a_ep, a_owner, a_p3, a_p4, a_p5);
		}

		g_killRetAddr = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
		g_killPending.store(true, std::memory_order_relaxed);

		const auto ret = g_origHandleEntryPoint(a_ep, a_owner, a_p3, a_p4, a_p5);

		return ret;
	}

	std::vector<std::uintptr_t> FindCallsTo(std::uintptr_t a_target)
	{
		std::vector<std::uintptr_t> sites;

		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (!base) {
			return sites;
		}

		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return sites;
		}

		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			return sites;
		}

		auto* sec = IMAGE_FIRST_SECTION(nt);
		for (std::uint32_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
			if (std::memcmp(sec->Name, ".text", 6) != 0) {
				continue;
			}

			const auto* begin = reinterpret_cast<const std::uint8_t*>(base + sec->VirtualAddress);
			const auto* end   = begin + sec->Misc.VirtualSize;

			for (const auto* p = begin; p + 5 <= end; ++p) {
				if (p[0] != 0xE8) {
					continue;
				}

				const auto tgt = reinterpret_cast<std::uintptr_t>(p) + 5 +
					*reinterpret_cast<const std::int32_t*>(p + 1);
				if (tgt == a_target) {
					sites.push_back(reinterpret_cast<std::uintptr_t>(p));
				}
			}

			break;
		}

		return sites;
	}

	bool IsGrimReaperSite(std::uintptr_t a_site, std::uintptr_t a_target)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(a_site);
		if (p[0] != 0xE8) {
			return false;
		}

		const auto tgt = a_site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
		if (tgt != a_target) {
			return false;
		}

		for (std::size_t d = 1; d <= 32; ++d) {
			const auto* q = p - d;
			if (q[0] == 0xB9 && q[1] == 0x6B && q[2] == 0x00 && q[3] == 0x00 && q[4] == 0x00) {
				return true;
			}
		}

		return false;
	}

	bool CallSiteLooksPatched(std::uintptr_t a_addr)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
		if (p[0] == 0xE9) {
			return true;
		}
		if (p[0] == 0xFF && p[1] == 0x25) {
			return true;
		}
		if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) {
			return true;
		}
		return false;
	}

	std::size_t InstallHandleEntryPointHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RE::ID::BGSEntryPoint::HandleEntryPoint };
		const auto tgt = target.address();
		if (!tgt) {
			return 0;
		}

		g_origHandleEntryPoint = reinterpret_cast<HandleEntryPointFn>(tgt);

		const auto sites = FindCallsTo(tgt);
		if (sites.empty()) {
			return 0;
		}

		std::vector<std::uintptr_t> grim;
		for (const auto site : sites) {
			if (IsGrimReaperSite(site, tgt)) {
				grim.push_back(site);
			}
		}

		if (grim.size() != 1) {
			return 0;
		}

		const auto site = grim.front();
		if (CallSiteLooksPatched(site)) {
			return 0;
		}

		REL::Relocation<std::uintptr_t> rel{ site };
		rel.write_call<5>(&HookedHandleEntryPoint);
		return 1;
	}

	void TopUpToRealMax(RE::PlayerCharacter* a_player, const RE::ActorValueInfo* a_info)
	{
		const auto av = RE::ActorValue::GetSingleton();
		if (!a_player || !av || !av->actionPoints || a_info != av->actionPoints) {
			return;
		}

		auto*       avo = static_cast<RE::ActorValueOwner*>(a_player);
		const auto  dmg = avo->GetModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, *a_info);
		if (dmg >= -0.5F) {
			return;
		}

		avo->RestoreActorValue(*a_info, -dmg);
	}

	bool IsPlayerAP(RE::ActorValueOwner* a_this, const RE::ActorValueInfo* a_info)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_info) {
			return false;
		}
		if (a_this != static_cast<RE::ActorValueOwner*>(player)) {
			return false;
		}

		const auto av = RE::ActorValue::GetSingleton();
		return av && av->actionPoints && a_info == av->actionPoints;
	}

	void OnAPRestored(RE::ActorValueOwner* a_this, const RE::ActorValueInfo* a_info,
		std::uintptr_t a_retAddr)
	{
		if (!g_killPending.load(std::memory_order_relaxed) || g_killRetAddr == 0) {
			return;
		}

		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || a_this != static_cast<RE::ActorValueOwner*>(player)) {
			return;
		}

		if (a_retAddr <= g_killRetAddr || a_retAddr - g_killRetAddr > 0x400) {
			return;
		}

		g_killPending.store(false, std::memory_order_relaxed);
		TopUpToRealMax(player, a_info);
	}

	using RestoreAVFn = void (*)(RE::ActorValueOwner*, const RE::ActorValueInfo*, float);
	RestoreAVFn g_origRestoreAV{ nullptr };

	void HookedRestoreAV(RE::ActorValueOwner* a_this, const RE::ActorValueInfo* a_info, float a_amount)
	{
		if (!g_origRestoreAV) {
			return;
		}

		g_origRestoreAV(a_this, a_info, a_amount);

		if (IsPlayerAP(a_this, a_info)) {
			const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			OnAPRestored(a_this, a_info, ret);
		}
	}

	bool InstallRestoreAVHook()
	{
		const auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			REX::INFO("RestoreActorValue: 玩家尚未创建,未挂钩");
			return false;
		}

		const auto* avo  = static_cast<const RE::ActorValueOwner*>(player);
		const auto  vtbl = *reinterpret_cast<const std::uintptr_t*>(avo);
		if (!vtbl) {
			REX::INFO("RestoreActorValue: 读不到玩家的属性值虚表");
			return false;
		}

		REL::Relocation<std::uintptr_t> rel{ vtbl };
		g_origRestoreAV = reinterpret_cast<RestoreAVFn>(
			rel.write_vfunc(0x08, reinterpret_cast<std::uintptr_t>(&HookedRestoreAV)));

		if (!g_origRestoreAV) {
			REX::INFO("RestoreActorValue: write_vfunc 返回空,未挂钩");
			return false;
		}

		REX::INFO("RestoreActorValue: 已在玩家实际虚表 0x{:X} 的槽 08 挂钩", vtbl);
		return true;
	}
}

F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	const auto data = F4SE::PluginVersionData::GetSingleton();
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = data->GetPluginName().data();
	a_info->version = data->GetPluginVersion().pack();

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < REL::Version(F4SE::RUNTIME_1_10_163)) {
		return false;
	}

	return true;
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::InitInfo initInfo{};
	initInfo.trampoline = true;
	initInfo.trampolineSize = 4096;
	F4SE::Init(a_f4se, initInfo);

	REX::INFO("=== GrimReaperAPFix loaded ===");

	grim::InstallHandleEntryPointHooks();

	if (const auto* messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener([](F4SE::MessagingInterface::Message* a_msg) {
			if (a_msg->type != F4SE::MessagingInterface::kGameDataReady) {
				return;
			}

			grim::InstallRestoreAVHook();
			});
	}

	return true;
}
