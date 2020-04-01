#include "wasapi_player.h"

#include <functional>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <comdef.h>
#include <comip.h>
#include <avrt.h>
#include <ksmedia.h>
#include <chrono>
#include <assert.h>
#include <functiondiscoverykeys_devpkey.h>
#include <codecvt>
#include <locale>
#include "common/snap_exception.hpp"
#include "common/aixlog.hpp"

using namespace std;
using namespace std::chrono;
using namespace std::chrono_literals;

static constexpr auto LOG_TAG = "WASAPI";

template<typename T>
struct COMMemDeleter
{
	void operator() (T* obj)
	{
		if (obj != NULL)
		{
			CoTaskMemFree(obj);
			obj = NULL;
		}
	}
};

template<typename T>
using com_mem_ptr = unique_ptr<T, COMMemDeleter<T> >;

using com_handle = unique_ptr<void, function<BOOL(HANDLE)> >;

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
const IID IID_IAudioClock = __uuidof(IAudioClock);

_COM_SMARTPTR_TYPEDEF(IMMDevice,__uuidof(IMMDevice));
_COM_SMARTPTR_TYPEDEF(IMMDeviceCollection,__uuidof(IMMDeviceCollection));
_COM_SMARTPTR_TYPEDEF(IMMDeviceEnumerator,__uuidof(IMMDeviceEnumerator));
_COM_SMARTPTR_TYPEDEF(IAudioClient,__uuidof(IAudioClient));
_COM_SMARTPTR_TYPEDEF(IPropertyStore,__uuidof(IPropertyStore));

#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

EXTERN_C const PROPERTYKEY DECLSPEC_SELECTANY PKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

#define CHECK_HR(hres) if(FAILED(hres)){stringstream ss;ss<<"HRESULT fault status: "<<hex<<(hres)<<" line "<<dec<<__LINE__<<endl;throw SnapException(ss.str());}

WASAPIPlayer::WASAPIPlayer(const PcmDevice& pcmDevice, std::shared_ptr<Stream> stream)
	: Player(pcmDevice, stream)
{
	HRESULT hr = CoInitializeEx(
		NULL,
		COINIT_MULTITHREADED);
	CHECK_HR(hr);
}

WASAPIPlayer::~WASAPIPlayer()
{
	WASAPIPlayer::stop();
}

inline PcmDevice convertToDevice(int idx, IMMDevicePtr& device)
{
	HRESULT hr;
	PcmDevice desc;

	LPWSTR id = NULL;
	hr = device->GetId(&id);
	CHECK_HR(hr);

	IPropertyStorePtr properties = nullptr;
	hr = device->OpenPropertyStore(STGM_READ, &properties);

	PROPVARIANT deviceName;
	PropVariantInit(&deviceName);

	hr = properties->GetValue(PKEY_Device_FriendlyName, &deviceName);
	CHECK_HR(hr);

	desc.idx = idx;
	desc.name = wstring_convert<codecvt_utf8<wchar_t>, wchar_t>().to_bytes(id);
	desc.description = wstring_convert<codecvt_utf8<wchar_t>, wchar_t>().to_bytes(deviceName.pwszVal);
	
	CoTaskMemFree(id);
	
	return desc;
}

vector<PcmDevice> WASAPIPlayer::pcm_list()
{
	HRESULT hr;
	IMMDeviceCollectionPtr devices = nullptr;
	IMMDeviceEnumeratorPtr deviceEnumerator = nullptr;
	
	hr = CoInitializeEx(
		NULL,
		COINIT_MULTITHREADED);
	if (hr != CO_E_ALREADYINITIALIZED)
		CHECK_HR(hr);
	
	hr = CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL,
		CLSCTX_SERVER, IID_IMMDeviceEnumerator,
		(void**)&deviceEnumerator);
	CHECK_HR(hr);

	hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
	CHECK_HR(hr);

	UINT deviceCount;
	devices->GetCount(&deviceCount);

	if (deviceCount == 0)
		throw SnapException("no valid devices");
	
	vector<PcmDevice> deviceList;
	
	{
		IMMDevicePtr defaultDevice = nullptr;
		hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
		CHECK_HR(hr);

		auto dev = convertToDevice(0, defaultDevice);
		dev.name = "default";
		deviceList.push_back(dev);
	}
	
	for (UINT i = 0; i < deviceCount; ++i)
	{
		IMMDevicePtr device = nullptr;

		hr = devices->Item(i, &device);
		CHECK_HR(hr);
		deviceList.push_back(convertToDevice(i + 1, device));
	}

	return deviceList;
}

void WASAPIPlayer::worker()
{
  assert(sizeof(char) == sizeof(BYTE));
	
	HRESULT hr;

	// Create the format specifier
	com_mem_ptr<WAVEFORMATEX> waveformat((WAVEFORMATEX*)(CoTaskMemAlloc(sizeof(WAVEFORMATEX))));
	waveformat->wFormatTag      = WAVE_FORMAT_PCM;
	waveformat->nChannels       = stream_->getFormat().channels();
	waveformat->nSamplesPerSec  = stream_->getFormat().rate();
	waveformat->wBitsPerSample  = stream_->getFormat().bits();

	waveformat->nBlockAlign     = waveformat->nChannels * waveformat->wBitsPerSample / 8;
	waveformat->nAvgBytesPerSec = waveformat->nSamplesPerSec * waveformat->nBlockAlign;

	waveformat->cbSize          = 0;

	com_mem_ptr<WAVEFORMATEXTENSIBLE> waveformatExtended((WAVEFORMATEXTENSIBLE*)(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE))));
	waveformatExtended->Format                      = *waveformat;
	waveformatExtended->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
	waveformatExtended->Format.cbSize               = 22;
	waveformatExtended->Samples.wValidBitsPerSample = waveformat->wBitsPerSample;
	waveformatExtended->dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	waveformatExtended->SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;


	// Retrieve the device enumerator
	IMMDeviceEnumeratorPtr deviceEnumerator = nullptr;
	hr = CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL, 
		CLSCTX_SERVER, IID_IMMDeviceEnumerator,
		(void**)&deviceEnumerator);
	CHECK_HR(hr);

	// Register the default playback device (eRender for playback)
	IMMDevicePtr device = nullptr;
	if (pcmDevice_.idx == 0)
	{
		hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
		CHECK_HR(hr);
	}
	else
	{
		IMMDeviceCollectionPtr devices = nullptr;
		hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
		CHECK_HR(hr);

		devices->Item(pcmDevice_.idx-1, &device); // 1: device passed by user is EnumAudioEndpoints -1 (we add "default" at the top when showing the list)
	}

	// Activate the device
	IAudioClientPtr audioClient = nullptr;
	hr = device->Activate(IID_IAudioClient, CLSCTX_SERVER, NULL, (void**)&audioClient);
	CHECK_HR(hr);

	hr = audioClient->IsFormatSupported(
		AUDCLNT_SHAREMODE_EXCLUSIVE,
		&(waveformatExtended->Format),
		NULL);
	CHECK_HR(hr);
	
	// Get the device period
	REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
	hr = audioClient->GetDevicePeriod(NULL, &hnsRequestedDuration);
	CHECK_HR(hr);
	
	// Initialize the client at minimum latency
	hr = audioClient->Initialize(
		AUDCLNT_SHAREMODE_EXCLUSIVE,
		AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
		hnsRequestedDuration,
		hnsRequestedDuration,
		&(waveformatExtended->Format),
		NULL);
	if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
	{
		UINT32 alignedBufferSize;
		hr = audioClient->GetBufferSize(&alignedBufferSize);
		CHECK_HR(hr);
		audioClient.Attach(NULL, false);
		hnsRequestedDuration = (REFERENCE_TIME)((10000.0 * 1000 / waveformatExtended->Format.nSamplesPerSec * alignedBufferSize) + 0.5);
		hr = device->Activate(IID_IAudioClient, CLSCTX_SERVER, NULL, (void**)&audioClient);
		CHECK_HR(hr);
		hr = audioClient->Initialize(
			AUDCLNT_SHAREMODE_EXCLUSIVE,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			hnsRequestedDuration,
			hnsRequestedDuration,
			&(waveformatExtended->Format),
			NULL);
	}
	CHECK_HR(hr);

	// Register an event to refill the buffer
	com_handle eventHandle(CreateEvent(NULL, FALSE, FALSE, NULL), &::CloseHandle);
	if (eventHandle == NULL)
		CHECK_HR(E_FAIL);
	hr = audioClient->SetEventHandle(HANDLE(eventHandle.get()));
	CHECK_HR(hr);

	// Get size of buffer
	UINT32 bufferFrameCount;
	hr = audioClient->GetBufferSize(&bufferFrameCount);
	CHECK_HR(hr);

	// Get the rendering service
	IAudioRenderClient* renderClient = NULL;
	hr = audioClient->GetService(
		IID_IAudioRenderClient,
		(void**)&renderClient);
	CHECK_HR(hr);

	// Grab the clock service
	IAudioClock* clock = NULL;
	hr = audioClient->GetService(
		IID_IAudioClock,
		(void**)&clock);
	CHECK_HR(hr);

	// Boost our priority
	DWORD taskIndex = 0;
	com_handle taskHandle(AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex),
		&::AvRevertMmThreadCharacteristics);
	if (taskHandle == NULL)
		CHECK_HR(E_FAIL);

	// And, action!
	hr = audioClient->Start();
	CHECK_HR(hr);
	
	size_t bufferSize = bufferFrameCount * waveformatExtended->Format.nBlockAlign;
	BYTE* buffer;
	unique_ptr<char[]> queueBuffer(new char[bufferSize]);
	UINT64 position = 0, bufferPosition = 0, frequency;
	clock->GetFrequency(&frequency);
	
	while (active_)
	{
	  DWORD returnVal = WaitForSingleObject(eventHandle.get(), 2000);
		if (returnVal != WAIT_OBJECT_0)
		{
            //stop();
            LOG(INFO, LOG_TAG) << "Got timeout waiting for audio device callback\n";
            CHECK_HR(ERROR_TIMEOUT);
			
			hr = audioClient->Stop();
            CHECK_HR(hr);
            hr = audioClient->Reset();
            CHECK_HR(hr);

            while (active_ && !stream_->waitForChunk(std::chrono::milliseconds(100)))
                LOG(INFO, LOG_TAG) << "Waiting for chunk\n";

            hr = audioClient->Start();
            CHECK_HR(hr);
            bufferPosition = 0;
            break;
		}

		// Thread was sleeping above, double check that we are still running
		if (!active_)
			break;

		clock->GetPosition(&position, NULL);

		if (stream_->getPlayerChunk(queueBuffer.get(), microseconds(
		                                                      ((bufferPosition * 1000000) / waveformat->nSamplesPerSec) -
		                                                      ((position * 1000000) / frequency)),
		                            bufferFrameCount))
		{
			adjustVolume(queueBuffer.get(), bufferFrameCount);
			hr = renderClient->GetBuffer(bufferFrameCount, &buffer);
			CHECK_HR(hr);
			memcpy(buffer, queueBuffer.get(), bufferSize);
			hr = renderClient->ReleaseBuffer(bufferFrameCount, 0);
			CHECK_HR(hr);
			
			bufferPosition += bufferFrameCount;
		}
		else
		{
			std::clog << static_cast<AixLog::Severity>(INFO) << AixLog::Tag(LOG_TAG);
			LOG(INFO, LOG_TAG) << "Failed to get chunk\n";
			
			hr = audioClient->Stop();
			CHECK_HR(hr);
			hr = audioClient->Reset();
			CHECK_HR(hr);
			
			while (active_ && !stream_->waitForChunk(std::chrono::milliseconds(100)))
				LOG(INFO, LOG_TAG) << "Waiting for chunk\n";
			
			hr = audioClient->Start();
			CHECK_HR(hr);
			bufferPosition = 0;
		}
	}
}