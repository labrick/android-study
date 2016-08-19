/*
 * Copyright (C) 2013 The Android Open Source Project
 * Copyright (C) 2013 Broadcom Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.bluetooth;

import android.bluetooth.BluetoothDevice;
import android.os.ParcelUuid;

import android.bluetooth.IBluetoothGattCallback;
import android.bluetooth.IBluetoothGattServerCallback;
import android.bluetooth.IBluetoothGattExtrasCallback;

/**
 * API for interacting with BLE / GATT
 * @hide
 */
interface IBluetoothGatt {
    List<BluetoothDevice> getDevicesMatchingConnectionStates(in int[] states);

    void startScan(in int appIf, in boolean isServer);
    void startScanWithUuids(in int appIf, in boolean isServer, in ParcelUuid[] ids);
    void stopScan(in int appIf, in boolean isServer);

    void registerClient(in ParcelUuid appId, in IBluetoothGattCallback callback);
    void unregisterClient(in int clientIf);
    void clientConnect(in int clientIf, in String address, in boolean isDirect);
    void clientDisconnect(in int clientIf, in String address);
    void clientListen(in int clientIf, in boolean start);
    void setAdvData(in int clientIf, in boolean setScanRsp, in boolean inclName,
                            in boolean inclTxPower, in int minInterval, in int maxInterval,
                            in int appearance, in byte[] manufacturerData);
    void refreshDevice(in int clientIf, in String address);
    void discoverServices(in int clientIf, in String address);
    void readCharacteristic(in int clientIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in int authReq);
    void writeCharacteristic(in int clientIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in int writeType, in int authReq, in byte[] value);
    void readDescriptor(in int clientIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in int descrInstanceId, in ParcelUuid descrUuid,
                            in int authReq);
    void writeDescriptor(in int clientIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in int descrInstanceId, in ParcelUuid descrId,
                            in int writeType, in int authReq, in byte[] value);
    void registerForNotification(in int clientIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in boolean enable);
    void beginReliableWrite(in int clientIf, in String address);
    void endReliableWrite(in int clientIf, in String address, in boolean execute);
    void readRemoteRssi(in int clientIf, in String address);

    void registerServer(in ParcelUuid appId, in IBluetoothGattServerCallback callback);
    void unregisterServer(in int serverIf);
    void serverConnect(in int servertIf, in String address, in boolean isDirect);
    void serverDisconnect(in int serverIf, in String address);
    void beginServiceDeclaration(in int serverIf, in int srvcType,
                            in int srvcInstanceId, in int minHandles,
                            in ParcelUuid srvcId);
    void addIncludedService(in int serverIf, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId);
    void addCharacteristic(in int serverIf, in ParcelUuid charId,
                            in int properties, in int permissions);
    void addDescriptor(in int serverIf, in ParcelUuid descId,
                            in int permissions);
    void endServiceDeclaration(in int serverIf);
    void removeService(in int serverIf, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId);
    void clearServices(in int serverIf);
    void sendResponse(in int serverIf, in String address, in int requestId,
                            in int status, in int offset, in byte[] value);
    void sendNotification(in int serverIf, in String address, in int srvcType,
                            in int srvcInstanceId, in ParcelUuid srvcId,
                            in int charInstanceId, in ParcelUuid charId,
                            in boolean confirm, in byte[] value);

    /**
     * Broadcom Extras
     */
    void registerExtras(in int appIf, in IBluetoothGattExtrasCallback callback);
    void unregisterExtras(in int appIf);
    void monitorRssi(in int clientIf, in String address,
                            in int rssi_low, in int rssi_range, in int rssi_high);
    void serverListen(in int serverIf, in boolean start);

    void scanFilterAddDeviceAddress(String address, in byte type);
    void scanFilterAddSolicitUuid(in ParcelUuid id);
    void scanFilterAddLocalName(in String name);
    void scanFilterAddManufacturerData(in int company, in byte[] data);
    void scanFilterAddServiceChanged();
    void scanFilterClear();
}