// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/chrome_feature_list_creator.h"

#include <set>
#include <vector>

#include "base/base_switches.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/path_service.h"
#include "base/system/sys_info.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "cc/base/switches.h"
#include "chrome/browser/about_flags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/metrics/chrome_metrics_service_accessor.h"
#include "chrome/browser/metrics/chrome_metrics_services_manager_client.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/browser/prefs/chrome_pref_service_factory.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_result_codes.h"
#include "chrome/common/pref_names.h"
#include "components/flags_ui/flags_ui_pref_names.h"
#include "components/flags_ui/pref_service_flags_storage.h"
#include "components/language/core/browser/pref_names.h"
#include "components/metrics/clean_exit_beacon.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/metrics/metrics_state_manager.h"
#include "components/metrics_services_manager/metrics_services_manager.h"
#include "components/policy/core/common/policy_service.h"
#include "components/prefs/pref_registry.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service_factory.h"
#include "components/variations/pref_names.h"
#include "components/variations/service/variations_service.h"
#include "components/variations/variations_crash_keys.h"
#include "services/service_manager/embedder/result_codes.h"
#include "ui/base/resource/resource_bundle.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/dbus/dbus_helper.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chromeos/chromeos_paths.h"
#endif  // defined(OS_CHROMEOS)

namespace {

#if defined(OS_CHROMEOS)
void RegisterStubPathOverridesIfNecessary() {
  // These overrides need to occur before BrowserPolicyConnectorChromeOS
  // (for one) is created. The DCHECK ensures that is the case.
  DCHECK(!g_browser_process);

  base::FilePath user_data_dir;
  if (base::SysInfo::IsRunningOnChromeOS() ||
      !base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    return;
  }

  // Override some paths with stub locations so that cloud policy and enterprise
  // enrollment work on desktop builds, for ease of development.
  chromeos::RegisterStubPathOverrides(user_data_dir);
}
#endif  // defined(OS_CHROMEOS)

}  // namespace

ChromeFeatureListCreator::ChromeFeatureListCreator() = default;

ChromeFeatureListCreator::~ChromeFeatureListCreator() = default;

void ChromeFeatureListCreator::CreateFeatureList() {
  CreatePrefService();
  ConvertFlagsToSwitches();
  CreateMetricsServices();
  SetupMasterPrefs();
  SetupFieldTrials();
}

metrics_services_manager::MetricsServicesManagerClient*
ChromeFeatureListCreator::GetMetricsServicesManagerClient() {
  return metrics_services_manager_client_;
}

std::unique_ptr<PrefService> ChromeFeatureListCreator::TakePrefService() {
  return std::move(local_state_);
}

std::unique_ptr<metrics_services_manager::MetricsServicesManager>
ChromeFeatureListCreator::TakeMetricsServicesManager() {
  return std::move(metrics_services_manager_);
}

std::unique_ptr<policy::ChromeBrowserPolicyConnector>
ChromeFeatureListCreator::TakeChromeBrowserPolicyConnector() {
  return std::move(browser_policy_connector_);
}

std::unique_ptr<prefs::InProcessPrefServiceFactory>
ChromeFeatureListCreator::TakePrefServiceFactory() {
  return std::move(pref_service_factory_);
}

#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
std::unique_ptr<first_run::MasterPrefs>
ChromeFeatureListCreator::TakeMasterPrefs() {
  return std::move(master_prefs_);
}
#endif

void ChromeFeatureListCreator::CreatePrefService() {
  base::FilePath local_state_file;
  bool result =
      base::PathService::Get(chrome::FILE_LOCAL_STATE, &local_state_file);
  DCHECK(result);

  auto pref_registry = base::MakeRefCounted<PrefRegistrySimple>();
  RegisterLocalState(pref_registry.get());

#if defined(OS_CHROMEOS)
  RegisterStubPathOverridesIfNecessary();
  chromeos::PreEarlyInitDBus();
  browser_policy_connector_ =
      std::make_unique<policy::BrowserPolicyConnectorChromeOS>();
#else
  browser_policy_connector_ =
      std::make_unique<policy::ChromeBrowserPolicyConnector>();
#endif  // defined(OS_CHROMEOS)

  pref_service_factory_ =
      std::make_unique<prefs::InProcessPrefServiceFactory>();
  auto delegate = pref_service_factory_->CreateDelegate();
  delegate->InitPrefRegistry(pref_registry.get());

  local_state_ = chrome_prefs::CreateLocalState(
      local_state_file, browser_policy_connector_->GetPolicyService(),
      std::move(pref_registry), false, std::move(delegate),
      browser_policy_connector_.get());
}

void ChromeFeatureListCreator::ConvertFlagsToSwitches() {
#if !defined(OS_CHROMEOS)
  // Convert active flags into switches. This needs to be done before
  // ui::ResourceBundle::InitSharedInstanceWithLocale as some loaded resources
  // are affected by experiment flags (--touch-optimized-ui in particular). On
  // ChromeOS system level flags are applied from the device settings from the
  // session manager.
  DCHECK(!ui::ResourceBundle::HasSharedInstance());
  TRACE_EVENT0("startup", "ChromeFeatureListCreator::ConvertFlagsToSwitches");
  flags_ui::PrefServiceFlagsStorage flags_storage(local_state_.get());
  about_flags::ConvertFlagsToSwitches(&flags_storage,
                                      base::CommandLine::ForCurrentProcess(),
                                      flags_ui::kAddSentinels);
#endif  // !defined(OS_CHROMEOS)
}

void ChromeFeatureListCreator::SetupFieldTrials() {
  browser_field_trials_ = std::make_unique<ChromeBrowserFieldTrials>();

  // Initialize FieldTrialList to support FieldTrials. This is intentionally
  // leaked since it needs to live for the duration of the browser process and
  // there's no benefit in cleaning it up at exit.
  base::FieldTrialList* leaked_field_trial_list = new base::FieldTrialList(
      metrics_services_manager_.get()->CreateEntropyProvider());
  ANNOTATE_LEAKING_OBJECT_PTR(leaked_field_trial_list);
  ignore_result(leaked_field_trial_list);

  auto feature_list = std::make_unique<base::FeatureList>();

  // Associate parameters chosen in about:flags and create trial/group for them.
  flags_ui::PrefServiceFlagsStorage flags_storage(local_state_.get());
  std::vector<std::string> variation_ids =
      about_flags::RegisterAllFeatureVariationParameters(&flags_storage,
                                                         feature_list.get());

  std::set<std::string> unforceable_field_trials;
#if defined(OFFICIAL_BUILD)
  unforceable_field_trials.insert("SettingsEnforcement");
#endif  // defined(OFFICIAL_BUILD)

  variations::VariationsService* variations_service =
      metrics_services_manager_.get()->GetVariationsService();
  variations_service->SetupFieldTrials(
      cc::switches::kEnableGpuBenchmarking, switches::kEnableFeatures,
      switches::kDisableFeatures, unforceable_field_trials, variation_ids,
      std::move(feature_list), browser_field_trials_.get());
  variations::InitCrashKeys();

  // Initialize FieldTrialSynchronizer system, which is used to synchronize
  // field trial state with child process.
  field_trial_synchronizer_ = base::MakeRefCounted<FieldTrialSynchronizer>();
}

void ChromeFeatureListCreator::CreateMetricsServices() {
  auto client =
      std::make_unique<ChromeMetricsServicesManagerClient>(local_state_.get());
  metrics_services_manager_client_ = client.get();
  metrics_services_manager_ =
      std::make_unique<metrics_services_manager::MetricsServicesManager>(
          std::move(client));
}

void ChromeFeatureListCreator::SetupMasterPrefs() {
  master_prefs_apply_result_ = ApplyFirstRunPrefs();
}

int ChromeFeatureListCreator::ApplyFirstRunPrefs() {
// Android does first run in Java instead of native.
// Chrome OS has its own out-of-box-experience code.
#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  master_prefs_ = std::make_unique<first_run::MasterPrefs>();
  // On first run, we need to process the predictor preferences before the
  // browser's profile_manager object is created, but after ResourceBundle
  // is initialized.
  if (!first_run::IsChromeFirstRun())
    return service_manager::RESULT_CODE_NORMAL_EXIT;

  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir))
    return chrome::RESULT_CODE_MISSING_DATA;

  first_run::ProcessMasterPreferencesResult pmp_result =
      first_run::ProcessMasterPreferences(user_data_dir, master_prefs_.get());
  if (pmp_result == first_run::EULA_EXIT_NOW)
    return chrome::RESULT_CODE_EULA_REFUSED;

  // TODO(macourteau): refactor preferences that are copied from
  // master_preferences into local_state, as a "local_state" section in
  // master preferences. If possible, a generic solution would be preferred
  // over a copy one-by-one of specific preferences. Also see related TODO
  // in first_run.h.

  // Store the initial VariationsService seed in local state, if it exists
  // in master prefs.
  if (!master_prefs_->compressed_variations_seed.empty()) {
    local_state_->SetString(variations::prefs::kVariationsCompressedSeed,
                            master_prefs_->compressed_variations_seed);
    if (!master_prefs_->variations_seed_signature.empty()) {
      local_state_->SetString(variations::prefs::kVariationsSeedSignature,
                              master_prefs_->variations_seed_signature);
    }
    // Set the variation seed date to the current system time. If the user's
    // clock is incorrect, this may cause some field trial expiry checks to
    // not do the right thing until the next seed update from the server,
    // when this value will be updated.
    local_state_->SetInt64(variations::prefs::kVariationsSeedDate,
                           base::Time::Now().ToInternalValue());
  }

  if (!master_prefs_->suppress_default_browser_prompt_for_version.empty()) {
    local_state_->SetString(
        prefs::kBrowserSuppressDefaultBrowserPrompt,
        master_prefs_->suppress_default_browser_prompt_for_version);
  }

#if defined(OS_WIN)
  if (!master_prefs_->welcome_page_on_os_upgrade_enabled)
    local_state_->SetBoolean(prefs::kWelcomePageOnOSUpgradeEnabled, false);
#endif
#endif  // !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  return service_manager::RESULT_CODE_NORMAL_EXIT;
}
