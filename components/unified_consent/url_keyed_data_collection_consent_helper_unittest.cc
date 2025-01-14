// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/unified_consent/url_keyed_data_collection_consent_helper.h"

#include <vector>

#include "components/sync/driver/test_sync_service.h"
#include "components/sync/engine/cycle/sync_cycle_snapshot.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "components/unified_consent/pref_names.h"
#include "components/unified_consent/scoped_unified_consent.h"
#include "components/unified_consent/unified_consent_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace unified_consent {
namespace {

class TestSyncService : public syncer::TestSyncService {
 public:
  TestSyncService() { SetActiveDataTypes({}); }

  void AddActiveDataType(syncer::ModelType type) {
    syncer::ModelTypeSet active_types = GetActiveDataTypes();
    active_types.Put(type);
    SetActiveDataTypes(active_types);
  }

  void FireOnStateChangeOnAllObservers() {
    for (auto& observer : observers_)
      observer.OnStateChanged(this);
  }

  // syncer::TestSyncService:
  void AddObserver(syncer::SyncServiceObserver* observer) override {
    observers_.AddObserver(observer);
  }
  void RemoveObserver(syncer::SyncServiceObserver* observer) override {
    observers_.RemoveObserver(observer);
  }

 private:
  base::ObserverList<syncer::SyncServiceObserver>::Unchecked observers_;
};

class UrlKeyedDataCollectionConsentHelperTest
    : public testing::Test,
      public UrlKeyedDataCollectionConsentHelper::Observer {
 public:
  // testing::Test:
  void SetUp() override {
    UnifiedConsentService::RegisterPrefs(pref_service_.registry());
  }

  void OnUrlKeyedDataCollectionConsentStateChanged(
      UrlKeyedDataCollectionConsentHelper* consent_helper) override {
    state_changed_notifications.push_back(consent_helper->IsEnabled());
  }

 protected:
  sync_preferences::TestingPrefServiceSyncable pref_service_;
  std::vector<bool> state_changed_notifications;
  TestSyncService sync_service_;
};

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       AnonymizedDataCollection_UnifiedConsentEnabled) {
  ScopedUnifiedConsent scoped_unified_consent(
      UnifiedConsentFeatureState::kEnabledNoBump);
  std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
      UrlKeyedDataCollectionConsentHelper::
          NewAnonymizedDataCollectionConsentHelper(&pref_service_,
                                                   &sync_service_);
  helper->AddObserver(this);
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  pref_service_.SetBoolean(prefs::kUrlKeyedAnonymizedDataCollectionEnabled,
                           true);
  EXPECT_TRUE(helper->IsEnabled());
  ASSERT_EQ(1U, state_changed_notifications.size());
  EXPECT_TRUE(state_changed_notifications[0]);

  state_changed_notifications.clear();
  pref_service_.SetBoolean(prefs::kUrlKeyedAnonymizedDataCollectionEnabled,
                           false);
  EXPECT_FALSE(helper->IsEnabled());
  ASSERT_EQ(1U, state_changed_notifications.size());
  EXPECT_FALSE(state_changed_notifications[0]);
  helper->RemoveObserver(this);
}

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       AnonymizedDataCollection_UnifiedConsentDisabled) {
  ScopedUnifiedConsent scoped_unified_consent(
      UnifiedConsentFeatureState::kDisabled);
  std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
      UrlKeyedDataCollectionConsentHelper::
          NewAnonymizedDataCollectionConsentHelper(&pref_service_,
                                                   &sync_service_);
  helper->AddObserver(this);
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  sync_service_.AddActiveDataType(syncer::ModelType::HISTORY_DELETE_DIRECTIVES);
  sync_service_.FireOnStateChangeOnAllObservers();
  EXPECT_TRUE(helper->IsEnabled());
  EXPECT_EQ(1U, state_changed_notifications.size());
  helper->RemoveObserver(this);
}

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       AnonymizedDataCollection_UnifiedConsentDisabled_NullSyncService) {
  ScopedUnifiedConsent scoped_unified_consent(
      UnifiedConsentFeatureState::kDisabled);
  std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
      UrlKeyedDataCollectionConsentHelper::
          NewAnonymizedDataCollectionConsentHelper(&pref_service_,
                                                   nullptr /* sync_service */);
  EXPECT_FALSE(helper->IsEnabled());
}

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       PersonalizeddDataCollection_UnifiedConsentEnabled) {
  ScopedUnifiedConsent scoped_unified_consent(
      UnifiedConsentFeatureState::kEnabledNoBump);
  std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
      UrlKeyedDataCollectionConsentHelper::
          NewPersonalizedDataCollectionConsentHelper(&sync_service_);
  helper->AddObserver(this);
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  // Peronalized data collection is disabled when only USER_EVENTS are enabled.
  sync_service_.AddActiveDataType(syncer::ModelType::USER_EVENTS);
  sync_service_.FireOnStateChangeOnAllObservers();
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  // Peronalized data collection is disabled when only HISTORY_DELETE_DIRECTIVES
  // are enabled.
  sync_service_.SetActiveDataTypes({});
  sync_service_.AddActiveDataType(syncer::ModelType::HISTORY_DELETE_DIRECTIVES);
  sync_service_.FireOnStateChangeOnAllObservers();
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  // Personalized data collection is enabled iff USER_EVENTS and
  // HISTORY_DELETE_DIRECTIVES are enabled.
  sync_service_.SetActiveDataTypes({});
  sync_service_.AddActiveDataType(syncer::ModelType::HISTORY_DELETE_DIRECTIVES);
  sync_service_.AddActiveDataType(syncer::ModelType::USER_EVENTS);
  sync_service_.FireOnStateChangeOnAllObservers();
  EXPECT_TRUE(helper->IsEnabled());
  EXPECT_EQ(1U, state_changed_notifications.size());
  helper->RemoveObserver(this);
}

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       PersonalizedDataCollection_UnifiedConsentDisabled) {
  ScopedUnifiedConsent scoped_unified_consent(
      UnifiedConsentFeatureState::kDisabled);
  std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
      UrlKeyedDataCollectionConsentHelper::
          NewPersonalizedDataCollectionConsentHelper(&sync_service_);
  helper->AddObserver(this);
  EXPECT_FALSE(helper->IsEnabled());
  EXPECT_TRUE(state_changed_notifications.empty());

  sync_service_.AddActiveDataType(syncer::ModelType::HISTORY_DELETE_DIRECTIVES);
  sync_service_.FireOnStateChangeOnAllObservers();
  EXPECT_TRUE(helper->IsEnabled());
  EXPECT_EQ(1U, state_changed_notifications.size());
  helper->RemoveObserver(this);
}

TEST_F(UrlKeyedDataCollectionConsentHelperTest,
       PersonalizedDataCollection_NullSyncService) {
  {
    ScopedUnifiedConsent scoped_unified_consent(
        UnifiedConsentFeatureState::kDisabled);
    std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
        UrlKeyedDataCollectionConsentHelper::
            NewPersonalizedDataCollectionConsentHelper(
                nullptr /* sync_service */);
    EXPECT_FALSE(helper->IsEnabled());
  }
  {
    ScopedUnifiedConsent scoped_unified_consent(
        UnifiedConsentFeatureState::kEnabledNoBump);
    std::unique_ptr<UrlKeyedDataCollectionConsentHelper> helper =
        UrlKeyedDataCollectionConsentHelper::
            NewPersonalizedDataCollectionConsentHelper(
                nullptr /* sync_service */);
    EXPECT_FALSE(helper->IsEnabled());
  }
}

}  // namespace
}  // namespace unified_consent
