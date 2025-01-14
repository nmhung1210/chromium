// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_BLUETOOTH_TRAY_BLUETOOTH_HELPER_H_
#define ASH_SYSTEM_BLUETOOTH_TRAY_BLUETOOTH_HELPER_H_

#include <string>
#include <vector>

#include "ash/ash_export.h"
#include "base/macros.h"
#include "device/bluetooth/bluetooth_common.h"
#include "services/device/public/mojom/bluetooth_system.mojom.h"

namespace ash {

using BluetoothDeviceList = std::vector<device::mojom::BluetoothDeviceInfoPtr>;

// Maps UI concepts from the Bluetooth system tray (e.g. "Bluetooth is on") into
// device concepts ("Bluetooth adapter enabled"). Note that most Bluetooth
// device operations are asynchronous, hence the two step initialization.
//
// This is a temporary virtual class used during the migration to the new
// BluetoothSystem Mojo interface. Once the migration is over, we'll
// de-virtualize this class and remove its legacy implementation.
class TrayBluetoothHelper {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // Called when the state of Bluetooth in the system changes.
    virtual void OnBluetoothSystemStateChanged() {}

    // Called when a Bluetooth scan has started or stopped.
    virtual void OnBluetoothScanStateChanged() {}

    // Called when a device was added, removed, or changed.
    virtual void OnBluetoothDeviceListChanged() {}
  };

  TrayBluetoothHelper();
  virtual ~TrayBluetoothHelper();

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Initializes and gets the adapter asynchronously.
  virtual void Initialize() = 0;

  // Returns a list of available bluetooth devices.
  virtual BluetoothDeviceList GetAvailableBluetoothDevices() const = 0;

  // Requests bluetooth start discovering devices, which happens asynchronously.
  virtual void StartBluetoothDiscovering() = 0;

  // Requests bluetooth stop discovering devices.
  virtual void StopBluetoothDiscovering() = 0;

  // Connect to a specific bluetooth device.
  virtual void ConnectToBluetoothDevice(const std::string& address) = 0;

  // Returns the state of Bluetooth in the system e.g. has hardware support,
  // is enabled, etc.
  virtual device::mojom::BluetoothSystem::State GetBluetoothState() = 0;

  // Returns true if there is a Bluetooth radio present.
  bool IsBluetoothStateAvailable();

  // Changes bluetooth state to |enabled|. If the current state and |enabled|
  // are same, it does nothing. If they're different, it toggles the state and
  // records UMA.
  virtual void SetBluetoothEnabled(bool enabled) = 0;

  // Returns whether the delegate has initiated a bluetooth discovery session.
  virtual bool HasBluetoothDiscoverySession() = 0;

 protected:
  void NotifyBluetoothSystemStateChanged();
  void NotifyBluetoothScanStateChanged();
  void NotifyBluetoothDeviceListChanged();

  base::ObserverList<Observer> observers_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TrayBluetoothHelper);
};

}  // namespace ash

#endif  // ASH_SYSTEM_BLUETOOTH_TRAY_BLUETOOTH_HELPER_H_
