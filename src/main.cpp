#include "Utilities.h"

using namespace RE;
using std::unordered_map;
PlayerCharacter* p;
PlayerControls* pcon;
static Setting* fThrowDelay;
static float pinPullTime;
static float pinMaxTime;
static float cookTime;
static TESObjectWEAP* grenadeForm;
const static F4SE::TaskInterface* taskInterface;

class CookInputHandler {
public:
	typedef void (CookInputHandler::* FnProceeButtonEvent)(ButtonEvent* evn);

	void ProcessButtonEvent(ButtonEvent* evn) {
		bool interruptThrow = false;
		if (evn->value != FP_ZERO) {
			if (evn->heldDownSecs >= fThrowDelay->GetFloat()) {
				bool* isPinPulled = (bool*)((uintptr_t)this + 0x28);
				if (!*isPinPulled) {
					grenadeForm = nullptr;
					if (p->currentProcess && p->currentProcess->middleHigh) {
						p->currentProcess->middleHigh->equippedItemsLock.lock();
						for (auto it = p->currentProcess->middleHigh->equippedItems.begin(); it != p->currentProcess->middleHigh->equippedItems.end(); ++it) {
							if (it->equipIndex.index == 2) {
								TESObjectWEAP* wep = (TESObjectWEAP*)it->item.object;
								BGSProjectile* projBase = wep->weaponData.rangedData->overrideProjectile;
								if (projBase->data.explosionProximity == FP_ZERO && (projBase->data.flags & 0x4) != 0 && (projBase->data.flags & 0x20000) != 0) {
									grenadeForm = wep;
									pinMaxTime = projBase->data.explosionTimer;
									//_MESSAGE("Grenade Instance found. Max cook time %f. Proj flag %llx", pinMaxTime, projBase->data.flags);
								}
							}
						}
						p->currentProcess->middleHigh->equippedItemsLock.unlock();
					}
					if (grenadeForm) {
						pinPullTime = *F4::ptr_engineTime;
						//_MESSAGE("Pin pulled at %f secs", pinPullTime);
					}
				}
				else {
					if (grenadeForm) {
						if (*F4::ptr_engineTime - pinPullTime >= pinMaxTime) {
							evn->value = FP_ZERO;
							cookTime = pinMaxTime;
							BGSEquipIndex equipIndex;
							equipIndex.index = 2;
							TaskQueueInterface::GetSingleton()->QueueWeaponFire(grenadeForm, p, equipIndex, grenadeForm->weaponData.ammo);
							interruptThrow = true;
							*isPinPulled = false;
						}
					}
				}
			}
		}
		else {
			if (grenadeForm) {
				cookTime = *F4::ptr_engineTime - pinPullTime;
			}
		}
		FnProceeButtonEvent fn = fnHash.at(*(uintptr_t*)this);
		if (fn) {
			(this->*fn)(evn);
		}
		if (interruptThrow) {
			//F4::BGSAnimationSystemUtils::InitializeActorInstant(*p, false);
			p->NotifyAnimationGraphImpl("weapForceEquip");
		}
	}

	void HookSink() {
		uintptr_t vtable = *(uintptr_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnProceeButtonEvent fn = SafeWrite64Function(vtable + 0x40, &CookInputHandler::ProcessButtonEvent);
			fnHash.insert(std::pair<uintptr_t, FnProceeButtonEvent>(vtable, fn));
		}
	}

protected:
	static unordered_map<uintptr_t, FnProceeButtonEvent> fnHash;
};
unordered_map<uintptr_t, CookInputHandler::FnProceeButtonEvent> CookInputHandler::fnHash;

class CookedGrenadeProjectile : public GrenadeProjectile {
public:
	typedef void (CookedGrenadeProjectile::* FnHandle3DLoaded)();

	void HookedHandle3DLoaded() {
		FnHandle3DLoaded fn = fnHash.at(*(uintptr_t*)this);
		if (fn)
			(this->*fn)();

		if (this->shooter.get().get() == p && this->weaponSource.object == grenadeForm) {
			taskInterface->AddTask([this]() {
				this->explosionTimer -= cookTime;
				//_MESSAGE("Projectile found at %llx. Cook time %f secs, remaining timer %f secs", this, cookTime, this->explosionTimer);
				grenadeForm = nullptr;
			});
		}
	}

	static void HookHandle3DLoaded(uintptr_t addr) {
		FnHandle3DLoaded fn = SafeWrite64Function(addr + 0x740, &CookedGrenadeProjectile::HookedHandle3DLoaded);
		fnHash.insert(std::pair<uintptr_t, FnHandle3DLoaded>(addr, fn));
	}

protected:
	static unordered_map<uintptr_t, FnHandle3DLoaded> fnHash;
};

unordered_map<uintptr_t, CookedGrenadeProjectile::FnHandle3DLoaded> CookedGrenadeProjectile::fnHash;

class AnimationGraphEventWatcher {
public:
	typedef BSEventNotifyControl (AnimationGraphEventWatcher::* FnProcessEvent)(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* dispatcher);

	BSEventNotifyControl HookedProcessEvent(BSAnimationGraphEvent& evn, BSTEventSource<BSAnimationGraphEvent>* src) {
		if (grenadeForm && evn.animEvent == "staggerStop") {
			grenadeForm = nullptr;
			*(bool*)((uintptr_t)pcon->meleeThrowHandler + 0x28) = false;
		}
		FnProcessEvent fn = fnHash.at(*(uintptr_t*)this);
		return fn ? (this->*fn)(evn, src) : BSEventNotifyControl::kContinue;
	}

	void HookSink() {
		uintptr_t vtable = *(uintptr_t*)this;
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnProcessEvent fn = SafeWrite64Function(vtable + 0x8, &AnimationGraphEventWatcher::HookedProcessEvent);
			fnHash.insert(std::pair<uintptr_t, FnProcessEvent>(vtable, fn));
		}
	}

protected:
	static std::unordered_map<uintptr_t, FnProcessEvent> fnHash;
};
std::unordered_map<uintptr_t, AnimationGraphEventWatcher::FnProcessEvent> AnimationGraphEventWatcher::fnHash;

void InitializePlugin() {
	p = PlayerCharacter::GetSingleton();
	((AnimationGraphEventWatcher*)((uintptr_t)p + 0x38))->HookSink();
	pcon = PlayerControls::GetSingleton();
	fThrowDelay = INISettingCollection::GetSingleton()->GetSetting("fThrowDelay:Controls");
	if (fThrowDelay) {
		_MESSAGE("fThrowDelay:Controls found");
		((CookInputHandler*)pcon->meleeThrowHandler)->HookSink();
		uintptr_t addr = GrenadeProjectile::VTABLE[0].address();
		_MESSAGE("Patching GrenadeProjectile %llx", addr);
		CookedGrenadeProjectile::HookHandle3DLoaded(addr);
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface * a_f4se, F4SE::PluginInfo * a_info) {
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::warn);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical("loaded in editor"sv);
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface * a_f4se) {
	F4SE::Init(a_f4se);

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	taskInterface = F4SE::GetTaskInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		}
		else if (msg->type == F4SE::MessagingInterface::kPreLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
			grenadeForm = nullptr;
		}
	});

	return true;
}
