// Copyright 2020 Boring
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//	http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "RtlWow64.h"
#include "RtlNative.h"
#include <string>

static NTSTATUS NTAPI RtlpProbeWritePtr(PVOID64 dest, PVOID64 src, ULONG len) {
	__try {
		RtlCopyMemory(dest, src, len);
	}
	__except (AccessViolationExceptionFilter()) {
		return STATUS_ACCESS_VIOLATION;
	}
	return STATUS_SUCCESS;
}

PVOID64 ntdll64;
PVOID64 LdrGetDllHandle;
PVOID64 LdrGetProcedureAddress;

HANDLE RtlpWow64ExecutableHeap;

NTSTATUS NTAPI RtlpGetModuleHandleWow64(
	_Out_ PVOID64*__ptr64 ModuleHandle,
	_In_ LPCSTR ModuleName) {
	NTSTATUS status = STATUS_SUCCESS;
	HANDLE hProcess = GetCurrentProcess();
	PVOID64 result = nullptr;

	status = RtlpProbeWritePtr(
		ModuleHandle,
		&result,
		sizeof(result)
	);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (!DuplicateHandle(hProcess, hProcess, hProcess, &hProcess, 0, 0, 2)) {
		return STATUS_UNSUCCESSFUL;
	}

	// Get peb64
	PROCESS_BASIC_INFORMATION64 pbi64{};
	status = NtWow64QueryInformationProcess64(
		hProcess,
		PROCESSINFOCLASS::ProcessBasicInformation,
		&pbi64,
		sizeof(pbi64),
		nullptr
	);
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	// Read Ldr64
	PEB_LDR_DATA64* __ptr64 pLdr64 = nullptr;
	PEB_LDR_DATA64 ldr64{};
	ULONG64 len = 0;
	status = NtWow64ReadVirtualMemory64(
		hProcess,
		GetLdr64(pbi64.PebBaseAddress),
		&pLdr64,
		sizeof(pLdr64),
		&len
	);
	if (len != sizeof(pLdr64)) {
		status = STATUS_UNSUCCESSFUL;
	}
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	status = NtWow64ReadVirtualMemory64(
		hProcess,
		pLdr64,
		&ldr64,
		sizeof(ldr64),
		&len
	);
	if (len != sizeof(ldr64)) {
		status = STATUS_UNSUCCESSFUL;
	}
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	// Find Entry
	auto head = ULONG64(GetLdr64(pbi64.PebBaseAddress)) + offsetof(PEB_LDR_DATA64, InLoadOrderModuleList);
	auto entry = ldr64.InLoadOrderModuleList.Flink;
	LDR_DATA_TABLE_ENTRY64_SNAP data{};
	std::string moduleName(ModuleName);
	WCHAR* currentModuleNameBuffer = nullptr;
	ULONG bufferLength = 0;
	
	while (entry != head) {
		status = NtWow64ReadVirtualMemory64(
			hProcess,
			PVOID64(entry),
			&data,
			sizeof(data),
			&len
		);
		if (len != sizeof(data)) {
			status = STATUS_UNSUCCESSFUL;
		}
		if (!NT_SUCCESS(status)) {
			break;
		}

		if (data.BaseDllName.Length > bufferLength) {
			delete[]currentModuleNameBuffer;
			currentModuleNameBuffer = new WCHAR[bufferLength = data.BaseDllName.Length];
		}

		status = NtWow64ReadVirtualMemory64(
			hProcess,
			data.BaseDllName.Buffer,
			currentModuleNameBuffer,
			data.BaseDllName.Length,
			&len
		);
		if (len != data.BaseDllName.Length) {
			status = STATUS_UNSUCCESSFUL;
		}
		if (!NT_SUCCESS(status)) {
			break;
		}

		if (std::equal(moduleName.begin(), moduleName.end(), currentModuleNameBuffer)) {
			result = data.DllBase;
			break;
		}

		entry = data.InLoadOrderLinks.Flink;
	}
	delete[]currentModuleNameBuffer;

	if (NT_SUCCESS(status)) {
		if (result == nullptr) {
			status = STATUS_DLL_NOT_FOUND;
		}
		else {
			status = RtlpProbeWritePtr(
				ModuleHandle,
				&result,
				sizeof(result)
			);
		}
	}

	CloseHandle(hProcess);
	return status;
}

NTSTATUS NTAPI RtlpGetProcAddressWow64(
	_Out_ PVOID64* __ptr64 FunctionAddress,
	_In_ PVOID64 ModuleHandle,
	_In_ LPCSTR FunctionName) {
	NTSTATUS status = STATUS_SUCCESS;
	PVOID64 result = nullptr;
	HANDLE hProcess = GetCurrentProcess();

	status = RtlpProbeWritePtr(
		FunctionAddress,
		&result,
		sizeof(result)
	);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (!DuplicateHandle(hProcess, hProcess, hProcess, &hProcess, 0, 0, 2)) {
		return STATUS_UNSUCCESSFUL;
	}

	IMAGE_DOS_HEADER dosHeader{};
	IMAGE_NT_HEADERS64 ntHeader{};
	ULONG64 len = 0;

	// Read dos header
	status = NtWow64ReadVirtualMemory64(
		hProcess,
		ModuleHandle,
		&dosHeader,
		sizeof(dosHeader),
		&len
	);
	if (len != sizeof(dosHeader)) {
		status = STATUS_UNSUCCESSFUL;
	}
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	// Read NT headers
	status = NtWow64ReadVirtualMemory64(
		hProcess,
		PVOID64(ULONG64(ModuleHandle) + dosHeader.e_lfanew),
		&ntHeader,
		sizeof(ntHeader),
		&len
	);
	if (len != sizeof(ntHeader)) {
		status = STATUS_UNSUCCESSFUL;
	}
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	// Read export directory
	IMAGE_EXPORT_DIRECTORY exportDir{};
	auto& dataTable = ntHeader.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (dataTable.Size == 0 || dataTable.VirtualAddress == 0) {
		status = STATUS_PROCEDURE_NOT_FOUND;
	}
	else {
		status = NtWow64ReadVirtualMemory64(
			hProcess,
			PVOID64(ULONG64(ModuleHandle) + dataTable.VirtualAddress),
			&exportDir,
			sizeof(exportDir),
			&len
		);
		if (len != sizeof(exportDir)) {
			status = STATUS_UNSUCCESSFUL;
		}
	}
	if (!NT_SUCCESS(status)) {
		CloseHandle(hProcess);
		return status;
	}

	// Read tables
	DWORD* rvaTable = new DWORD[exportDir.NumberOfFunctions]{};
	WORD* ordTable = new WORD[exportDir.NumberOfFunctions]{};
	DWORD* nameTable = new DWORD[exportDir.NumberOfNames]{};
	VOID* tabs[3] = { rvaTable,ordTable,nameTable };
	BYTE lenPerTableElement[3] = { sizeof(DWORD),sizeof(WORD),sizeof(DWORD) };
	DWORD offsets[3] = { exportDir.AddressOfFunctions,exportDir.AddressOfNameOrdinals,exportDir.AddressOfNames };
	DWORD numbers[3] = { exportDir.NumberOfFunctions,exportDir.NumberOfFunctions,exportDir.NumberOfNames };
	for (int i = 0; i < 3; ++i) {
		status = NtWow64ReadVirtualMemory64(
			hProcess,
			PVOID64(ULONG64(ModuleHandle) + offsets[i]),
			tabs[i],
			ULONG64(lenPerTableElement[i]) * numbers[i],
			&len
		);
		if (len != ULONG64(lenPerTableElement[i]) * numbers[i]) {
			status = STATUS_UNSUCCESSFUL;
		}
		if (!NT_SUCCESS(status)) {
			break;
		}
	}

	if (NT_SUCCESS(status)) {
		auto bufferLen = strlen(FunctionName) + 1;
		LPSTR buffer = new CHAR[bufferLen];
		for (DWORD i = 0; i < exportDir.NumberOfNames; ++i) {
			status = NtWow64ReadVirtualMemory64(
				hProcess,
				PVOID64(ULONG64(ModuleHandle) + nameTable[i]),
				buffer,
				bufferLen,
				&len
			);
			if (len != bufferLen) {
				status = STATUS_UNSUCCESSFUL;
			}
			if (!NT_SUCCESS(status)) {
				break;
			}

			if (_stricmp(buffer, FunctionName) == 0) {
				result = PVOID64(ULONG64(ModuleHandle) + rvaTable[ordTable[i]]);
				break;
			}
		}
		delete[]buffer;
	}

	CloseHandle(hProcess);
	delete[]rvaTable;
	delete[]ordTable;
	delete[]nameTable;

	if (NT_SUCCESS(status)) {
		if (result == nullptr) {
			status = STATUS_PROCEDURE_NOT_FOUND;
		}
		else {
			status = RtlpProbeWritePtr(
				FunctionAddress,
				&result,
				sizeof(result)
			);
		}
	}

	return status;
}

static const unsigned char Wow64Execute[] = {
	//BITS32
	0x55,										//push ebp
	0x89, 0xe5,									//mov ebp, esp
	0x56,										//push esi
	0x57,										//push edi
	0x8b, 0x75, 0x08,							//mov esi, dword ptr ss:[ebp + 0x8]
	0x8b, 0x4d, 0x0c,							//mov ecx, dword ptr ss:[ebp + 0xC]
	0xe8, 0x00, 0x00, 0x00, 0x00,				//call $0
	0x58,										//pop eax
	0x83, 0xc0, 0x2a,							//add eax, 0x2A
	0x83, 0xec, 0x08,							//sub esp, 0x8
	0x89, 0xe2,									//mov edx, esp
	0xc7, 0x42, 0x04, 0x33, 0x00, 0x00, 0x00,	//mov dword ptr ds:[edx + 0x4], 0x33
	0x89, 0x02,									//mov dword ptr ds:[edx], eax
	0xe8, 0x0e, 0x00, 0x00, 0x00,				//call SwitchTo64
	0x66, 0x8c, 0xd9,							//mov cx, ds
	0x8e, 0xd1,									//mov ss, cx
	0x83, 0xc4, 0x14,							//add esp, 0x14
	0x5f,										//pop edi
	0x5e,										//pop esi
	0x5d,										//pop ebp
	0xc2, 0x08, 0x00,							//ret 0x8

	//SwitchTo64:
	0x8b, 0x3c, 0x24,							//mov edi, dword ptr ss:[esp]
	0xff, 0x2a,									//jmp far fword ptr ds:[edx]


	//BITS64
	0x48, 0x31, 0xc0,							//xor rax, rax
	0x57,										//push rdi
	0xff, 0xd6,									//call rsi
	0x5f,										//pop rdi
	0x50,										//push rax
	0xc7, 0x44, 0x24, 0x04, 0x23, 0x00, 0x00, 0x00,//mov dword ptr ss:[rsp + 0x4], 0x23
	0x89, 0x3c, 0x24,							//mov dword ptr ss:[rsp], edi
	0x48, 0x89, 0xC2,							//mov rdx, rax
	0x21, 0xC0,									//and eax, eax
	0x48, 0xC1, 0xEA, 0x20,						//shr rdx, 0x20
	0xff, 0x2c, 0x24,							//jmp far fword ptr ss:[rsp]
};
ULONG64 NTAPI RtlpDispatchX64Call(
	_In_ std::string& code,
	_In_ const ULONG64* parameters) {
	using Wow64Execution = ULONG64(NTAPI*)(LPCVOID lpFunc, LPCVOID lpParameter);
	ULONG64 result = -1;
	LPBYTE pExecutableCode = nullptr;

	if (code.empty())return result;

	pExecutableCode = (LPBYTE)RtlAllocateHeap(RtlpWow64ExecutableHeap, HEAP_ZERO_MEMORY, sizeof(Wow64Execute) + code.size());
	if (!pExecutableCode) return result;

	RtlCopyMemory(pExecutableCode, Wow64Execute, sizeof(Wow64Execute));
	RtlCopyMemory(pExecutableCode + sizeof(Wow64Execute), code.data(), code.size());

	result = (Wow64Execution(pExecutableCode))(pExecutableCode + sizeof(Wow64Execute), parameters);
	RtlFreeHeap(RtlpWow64ExecutableHeap, 0, pExecutableCode);

	return result;
}

ULONG64 NTAPI RtlpWow64Execute64(
	_In_ PVOID64 Function,
	_In_reads_bytes_(dwParameters * sizeof(ULONG64)) const ULONG64* pFunctionParameters,
	_In_ DWORD dwParameters) {
	//BITS 64
	const unsigned char prologue[] = {
		0xFC,										//cld
		0x48, 0x89, 0xCE,							//mov rsi, rcx
		0x48, 0x89, 0xE7,							//mov rdi, rsp
		0x48, 0x83, 0xEC, 0x10,						//sub rsp, 0x10
		0x48, 0x81, 0xE4, 0xF0, 0xFF, 0xFF, 0xFF,	//and rsp, 0xFFFFFFFFFFFFFFF0
	};

	//BITS 64
	unsigned char epilogue[] = {
		0x31, 0xC0,														//xor eax, eax
		0x49, 0xBA, 0xF1, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,		//mov r10, FunctionAddress
		0x41, 0xFF, 0xD2,												//call r10
		0x48, 0x89, 0xFC,												//mov rsp, rdi
		0xC3															//ret
	};

	std::string code(LPCSTR(prologue), sizeof(prologue));

	if (dwParameters < 4) {
		auto c = dwParameters < 4 ? dwParameters : 4;
		for (DWORD i = 0; i < c; ++i) {
			switch (i) {
			case 0:
				//mov rcx, qword ptr ds:[rsi]
				code.append("\x48\x8B\x0E", 3);
				break;
			case 1:
				//mov rdx, qword ptr ds:[rsi + 0x8]
				code.append("\x48\x8B\x56\x08", 4);
				break;
			case 2:
				//mov r8, qword ptr ds:[rsi + 0x10]
				code.append("\x4C\x8B\x46\x10", 4);
				break;
			case 3:
				//mov r9, qword ptr ds:[rsi + 0x18]
				code.append("\x4C\x8B\x4E\x18", 4);
				break;
			}
		}
	}
	else {
		code.append("\x48\x8B\x0E""\x48\x8B\x56\x08""\x4C\x8B\x46\x10""\x4C\x8B\x4E\x18", 15);

		//mov rax, qword ptr ds:[rsi + 8*i]
		//push rax
		CHAR code_buffer1[] = "\x48\x8B\x46\x20\x50",
			code_buffer2[] = "\x48\x8B\x86\x80\x00\x00\x00\x50";

		if (dwParameters * 8 >= 0x7fffffff)return -1;
		//if (dwParameters % 2 == 0)code.append("\x50", 1);
		for (DWORD i = dwParameters - 1; i >= 4; --i) {
			if (i * 8 < 0x7f) {
				code_buffer1[3] = i * 8;
				code.append(code_buffer1, 5);
			}
			else {
				*LPDWORD(code_buffer2 + 3) = i * 8;
				code.append(code_buffer2, 8);
			}
		}
	}

	*PULONG64(epilogue + 4) = ULONG64(Function);
	code.append(LPCSTR(epilogue), sizeof(epilogue));

	return RtlpDispatchX64Call(code, pFunctionParameters);
}

NTSTATUS NTAPI RtlGetModuleHandleWow64(
	_Out_ PVOID64* __ptr64 ModuleHandle,
	_In_ LPCSTR ModuleName) {
	UNICODE_STRING64 str{};
	ULONG64 p[5] = { 0,0,ULONG64(&str),ULONG64(ModuleHandle) };

	RtlCreateUnicodeString64FromAsciiz(&str, ModuleName);

	RtlInvokeX64(&p[4], LdrGetDllHandle, PULONG64(&p[0]), 4);

	RtlFreeUnicodeString64(&str);

	return NTSTATUS(p[4]);
}

NTSTATUS NTAPI RtlGetProcAddressWow64(
	_Out_ PVOID64* __ptr64 FunctionAddress,
	_In_ PVOID64 ModuleHandle,
	_In_ LPCSTR FunctionName) {
	ANSI_STRING64 str{};
	ULONG64 p[5] = { ULONG64(ModuleHandle),ULONG64(&str),0,ULONG64(FunctionAddress) };

	RtlInitAnsiString64(&str, FunctionName);

	RtlInvokeX64(&p[4], LdrGetProcedureAddress, PULONG64(&p[0]), 4);
	return NTSTATUS(p[4]);
}

NTSTATUS NTAPI RtlInvokeX64(
	_Out_opt_ PULONG64 Result,
	_In_ PVOID64 FunctionAddress,
	_In_ ULONG64* Parameters,
	_In_ DWORD ParameterCount) {
	auto result = RtlpWow64Execute64(FunctionAddress, Parameters, ParameterCount);
	if (Result) {
		*Result = result;
	}
	return STATUS_SUCCESS;
}
