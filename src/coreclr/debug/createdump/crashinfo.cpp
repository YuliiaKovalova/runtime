// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "createdump.h"

// This is for the PAL_VirtualUnwindOutOfProc read memory adapter.
CrashInfo* g_crashInfo;

static bool ModuleInfoCompare(const ModuleInfo* lhs, const ModuleInfo* rhs) { return lhs->BaseAddress() < rhs->BaseAddress(); }

CrashInfo::CrashInfo(pid_t pid, bool gatherFrames, pid_t crashThread, uint32_t signal) :
    m_ref(1),
    m_pid(pid),
    m_ppid(-1),
    m_hdac(nullptr),
    m_pClrDataEnumRegions(nullptr),
    m_pClrDataProcess(nullptr),
    m_gatherFrames(gatherFrames),
    m_crashThread(crashThread),
    m_signal(signal),
    m_moduleInfos(&ModuleInfoCompare),
    m_mainModule(nullptr),
    m_cbModuleMappings(0),
    m_dataTargetPagesAdded(0),
    m_enumMemoryPagesAdded(0)
{
    g_crashInfo = this;
#ifdef __APPLE__
    m_task = 0;
#else
    m_auxvValues.fill(0);
    m_fd = -1;
#endif
}

CrashInfo::~CrashInfo()
{
    // Clean up the threads
    for (ThreadInfo* thread : m_threads)
    {
        delete thread;
    }
    m_threads.clear();

    // Clean up the modules
    for (ModuleInfo* module : m_moduleInfos)
    {
        delete module;
    }
    m_moduleInfos.clear();

    // Clean up DAC interfaces
    if (m_pClrDataEnumRegions != nullptr)
    {
        m_pClrDataEnumRegions->Release();
    }
    if (m_pClrDataProcess != nullptr)
    {
        m_pClrDataProcess->Release();
    }
    // Unload DAC module
    if (m_hdac != nullptr)
    {
        FreeLibrary(m_hdac);
        m_hdac = nullptr;
    }
#ifdef __APPLE__
    if (m_task != 0)
    {
        kern_return_t result = ::mach_port_deallocate(mach_task_self(), m_task);
        if (result != KERN_SUCCESS)
        {
            printf_error("Internal error: mach_port_deallocate FAILED %s (%x)\n", mach_error_string(result), result);
        }
    }
#endif
}

STDMETHODIMP
CrashInfo::QueryInterface(
    ___in REFIID InterfaceId,
    ___out PVOID* Interface)
{
    if (InterfaceId == IID_IUnknown ||
        InterfaceId == IID_ICLRDataEnumMemoryRegionsCallback)
    {
        *Interface = (ICLRDataEnumMemoryRegionsCallback*)this;
        AddRef();
        return S_OK;
    }
    else
    {
        *Interface = nullptr;
        return E_NOINTERFACE;
    }
}

STDMETHODIMP_(ULONG)
CrashInfo::AddRef()
{
    LONG ref = InterlockedIncrement(&m_ref);
    return ref;
}

STDMETHODIMP_(ULONG)
CrashInfo::Release()
{
    LONG ref = InterlockedDecrement(&m_ref);
    if (ref == 0)
    {
        delete this;
    }
    return ref;
}

HRESULT STDMETHODCALLTYPE
CrashInfo::EnumMemoryRegion(
    /* [in] */ CLRDATA_ADDRESS address,
    /* [in] */ ULONG32 size)
{
    m_enumMemoryPagesAdded += InsertMemoryRegion((ULONG_PTR)address, size);
    return S_OK;
}

//
// Gather all the necessary crash dump info.
//
bool
CrashInfo::GatherCrashInfo(MINIDUMP_TYPE minidumpType)
{
    // Get the info about the threads (registers, etc.)
    for (ThreadInfo* thread : m_threads)
    {
        if (!thread->Initialize())
        {
            return false;
        }
    }
#ifdef __APPLE__
    if (!EnumerateMemoryRegions())
    {
        return false;
    }
#else
    // Get the auxv data
    if (!GetAuxvEntries())
    {
        return false;
    }
    // Gather all the module memory mappings (from /dev/$pid/maps)
    if (!EnumerateMemoryRegions())
    {
        return false;
    }
    // Get shared module debug info
    if (!GetDSOInfo())
    {
        return false;
    }
#endif
    // Load and initialize DAC interfaces
    if (!InitializeDAC())
    {
        return false;
    }
    // Enumerate all the managed modules. On MacOS only the native modules have been added
    // to the module mapping list at this point and adds the managed modules. This needs to
    // be done before the other mappings is initialized.
    if (!EnumerateManagedModules())
    {
        return false;
    }
#ifdef __APPLE__
    InitializeOtherMappings();
#endif
    if (!UnwindAllThreads())
    {
        return false;
    }
    if (g_diagnosticsVerbose)
    {
        TRACE("Module addresses:\n");
        for (const MemoryRegion& region : m_moduleAddresses)
        {
            region.Trace();
        }
    }
    // If full memory dump, include everything regardless of permissions
    if (minidumpType & MiniDumpWithFullMemory)
    {
        for (const MemoryRegion& region : m_moduleMappings)
        {
            InsertMemoryRegion(region);
        }
        for (const MemoryRegion& region : m_otherMappings)
        {
            // Don't add uncommitted pages to the full dump
            if ((region.Permissions() & (PF_R | PF_W | PF_X)) != 0)
            {
                InsertMemoryRegion(region);
            }
        }
    }
    else
    {
        // Add all the heap read/write memory regions (m_otherMappings contains the heaps). On Alpine
        // the heap regions are marked RWX instead of just RW.
        if (minidumpType & MiniDumpWithPrivateReadWriteMemory)
        {
            for (const MemoryRegion& region : m_otherMappings)
            {
                uint32_t permissions = region.Permissions();
#ifdef __APPLE__
                if (permissions == (PF_R | PF_W))
#else
                if (permissions == (PF_R | PF_W) || permissions == (PF_R | PF_W | PF_X))
#endif
                {
                    InsertMemoryRegion(region);
                }
            }
        }
        // Add the thread's stack and some code memory to core
        for (ThreadInfo* thread : m_threads)
        {
            // Add the thread's stack
            thread->GetThreadStack();
        }
    }
    return true;
}

static const char*
GetHResultString(HRESULT hr)
{
    switch (hr)
    {
        case E_FAIL:
            return "The operation has failed";
        case E_INVALIDARG:
            return "Invalid argument";
        case E_OUTOFMEMORY:
            return "Out of memory";
        case CORDBG_E_INCOMPATIBLE_PLATFORMS:
            return "The operation failed because debuggee and debugger are on incompatible platforms";
        case CORDBG_E_MISSING_DEBUGGER_EXPORTS:
            return "The debuggee memory space does not have the expected debugging export table";
        case CORDBG_E_UNSUPPORTED:
            return "The specified action is unsupported by this version of the runtime";
    }
    return "";
}

//
// Enumerate all the memory regions using the DAC memory region support given a minidump type
//
bool
CrashInfo::InitializeDAC()
{
    ReleaseHolder<DumpDataTarget> dataTarget = new DumpDataTarget(*this);
    PFN_CLRDataCreateInstance pfnCLRDataCreateInstance = nullptr;
    bool result = false;
    HRESULT hr = S_OK;

    if (!m_coreclrPath.empty())
    {
        // We assume that the DAC is in the same location as the libcoreclr.so module
        std::string dacPath;
        dacPath.append(m_coreclrPath);
        dacPath.append(MAKEDLLNAME_A("mscordaccore"));

        // Load and initialize the DAC
        m_hdac = LoadLibraryA(dacPath.c_str());
        if (m_hdac == nullptr)
        {
            printf_error("InitializeDAC: LoadLibraryA(%s) FAILED %s\n", dacPath.c_str(), GetLastErrorString().c_str());
            goto exit;
        }
        pfnCLRDataCreateInstance = (PFN_CLRDataCreateInstance)GetProcAddress(m_hdac, "CLRDataCreateInstance");
        if (pfnCLRDataCreateInstance == nullptr)
        {
            printf_error("InitializeDAC: GetProcAddress(CLRDataCreateInstance) FAILED %s\n", GetLastErrorString().c_str());
            goto exit;
        }
        hr = pfnCLRDataCreateInstance(__uuidof(ICLRDataEnumMemoryRegions), dataTarget, (void**)&m_pClrDataEnumRegions);
        if (FAILED(hr))
        {
            printf_error("InitializeDAC: CLRDataCreateInstance(ICLRDataEnumMemoryRegions) FAILED %s (%08x)\n", GetHResultString(hr), hr);
            goto exit;
        }
        hr = pfnCLRDataCreateInstance(__uuidof(IXCLRDataProcess), dataTarget, (void**)&m_pClrDataProcess);
        if (FAILED(hr))
        {
            printf_error("InitializeDAC: CLRDataCreateInstance(IXCLRDataProcess) FAILED %s (%08x)\n", GetHResultString(hr), hr);
            goto exit;
        }
    }
    else
    {
        printf_error("InitializeDAC: coreclr not found; not using DAC\n");
    }
    result = true;
exit:
    return result;
}

//
// Enumerate all the memory regions using the DAC memory region support given a minidump type
//
bool
CrashInfo::EnumerateMemoryRegionsWithDAC(MINIDUMP_TYPE minidumpType)
{
    if (m_pClrDataEnumRegions != nullptr && (minidumpType & MiniDumpWithFullMemory) == 0)
    {
        TRACE("EnumerateMemoryRegionsWithDAC: Memory enumeration STARTED (%d %d)\n", m_enumMemoryPagesAdded, m_dataTargetPagesAdded);

        // Since on both Linux and MacOS all the RW regions will be added for heap
        // dumps by createdump, the only thing differentiating a MiniDumpNormal and
        // a MiniDumpWithPrivateReadWriteMemory is that the later uses the EnumMemory
        // APIs. This is kind of expensive on larger applications (4 minutes, or even
        // more), and this should already be in RW pages. Change the dump type to the
        // faster normal one. This one already ensures necessary DAC globals, etc.
        // without the costly assembly, module, class, type runtime data structures
        // enumeration.
        if (minidumpType & MiniDumpWithPrivateReadWriteMemory)
        {
            char* fastHeapDumps = getenv("COMPlus_DbgEnableFastHeapDumps");
            if (fastHeapDumps != nullptr && strcmp(fastHeapDumps, "1") == 0)
            {
                minidumpType = MiniDumpNormal;
            }
        }
        // Calls CrashInfo::EnumMemoryRegion for each memory region found by the DAC
        HRESULT hr = m_pClrDataEnumRegions->EnumMemoryRegions(this, minidumpType, CLRDATA_ENUM_MEM_DEFAULT);
        if (FAILED(hr))
        {
            printf_error("EnumMemoryRegions FAILED %s (%08x)\n", GetHResultString(hr), hr);
            return false;
        }
        TRACE("EnumerateMemoryRegionsWithDAC: Memory enumeration FINISHED (%d %d)\n", m_enumMemoryPagesAdded, m_dataTargetPagesAdded);
    }
    return true;
}

//
// Enumerate all the managed modules and replace the module mapping with the module name found.
//
bool
CrashInfo::EnumerateManagedModules()
{
    CLRDATA_ENUM enumModules = 0;
    HRESULT hr = S_OK;

    if (m_pClrDataProcess != nullptr)
    {
        TRACE("EnumerateManagedModules: Module enumeration STARTED (%d)\n", m_dataTargetPagesAdded);

        if (FAILED(hr = m_pClrDataProcess->StartEnumModules(&enumModules))) {
            printf_error("StartEnumModules FAILED %s (%08x)\n", GetHResultString(hr), hr);
            return false;
        }

        while (true)
        {
            ReleaseHolder<IXCLRDataModule> pClrDataModule;
            if ((hr = m_pClrDataProcess->EnumModule(&enumModules, &pClrDataModule)) != S_OK) {
                break;
            }

            // Skip any dynamic modules. The Request call below on some DACs crashes on dynamic modules.
            ULONG32 flags;
            if ((hr = pClrDataModule->GetFlags(&flags)) != S_OK) {
                TRACE("MODULE: GetFlags FAILED %08x\n", hr);
                continue;
            }
            if (flags & CLRDATA_MODULE_IS_DYNAMIC) {
                TRACE("MODULE: Skipping dynamic module\n");
                continue;
            }

            DacpGetModuleData moduleData;
            if (SUCCEEDED(hr = moduleData.Request(pClrDataModule.GetPtr())))
            {
                TRACE("MODULE: %" PRIA PRIx64 " dyn %d inmem %d file %d pe %" PRIA PRIx64 " pdb %" PRIA PRIx64, (uint64_t)moduleData.LoadedPEAddress, moduleData.IsDynamic,
                    moduleData.IsInMemory, moduleData.IsFileLayout, (uint64_t)moduleData.PEAssembly, (uint64_t)moduleData.InMemoryPdbAddress);

                if (!moduleData.IsDynamic && moduleData.LoadedPEAddress != 0)
                {
                    ArrayHolder<WCHAR> wszUnicodeName = new WCHAR[MAX_LONGPATH + 1];
                    if (SUCCEEDED(hr = pClrDataModule->GetFileName(MAX_LONGPATH, nullptr, wszUnicodeName)))
                    {
                        std::string moduleName = FormatString("%S", wszUnicodeName.GetPtr());

                        // Change the module mapping name
                        AddOrReplaceModuleMapping(moduleData.LoadedPEAddress, moduleData.LoadedPESize, moduleName);

                        // Add managed module info
                        AddModuleInfo(true, moduleData.LoadedPEAddress, pClrDataModule, moduleName);
                    }
                    else {
                        TRACE("\nModule.GetFileName FAILED %08x\n", hr);
                    }
                }
                else {
                    TRACE("\n");
                }
            }
            else {
                TRACE("moduleData.Request FAILED %08x\n", hr);
            }
        }

        if (enumModules != 0) {
            m_pClrDataProcess->EndEnumModules(enumModules);
        }
        TRACE("EnumerateManagedModules: Module enumeration FINISHED (%d) ModuleMappings %06llx\n", m_dataTargetPagesAdded, m_cbModuleMappings / PAGE_SIZE);
    }
    return true;
}

//
// Unwind all the native threads to ensure that the dwarf unwind info is added to the core dump.
//
bool
CrashInfo::UnwindAllThreads()
{
    TRACE("UnwindAllThreads: STARTED (%d)\n", m_dataTargetPagesAdded);
    ReleaseHolder<ISOSDacInterface> pSos = nullptr;
    if (m_pClrDataProcess != nullptr) {
        m_pClrDataProcess->QueryInterface(__uuidof(ISOSDacInterface), (void**)&pSos);
    }
    // For each native and managed thread
    for (ThreadInfo* thread : m_threads)
    {
        if (!thread->UnwindThread(m_pClrDataProcess, pSos)) {
            return false;
        }
    }
    TRACE("UnwindAllThreads: FINISHED (%d)\n", m_dataTargetPagesAdded);
    return true;
}

//
// Replace an existing module mapping with one with a different name.
//
void
CrashInfo::AddOrReplaceModuleMapping(CLRDATA_ADDRESS baseAddress, ULONG64 size, const std::string& name)
{
    // Round to page boundary (single-file managed assemblies are not page aligned)
    ULONG_PTR start = ((ULONG_PTR)baseAddress) & PAGE_MASK;
    assert(start > 0);

    // Round up to page boundary
    ULONG_PTR end = ((baseAddress + size) + (PAGE_SIZE - 1)) & PAGE_MASK;
    assert(end > 0);

    uint32_t flags = GetMemoryRegionFlags((ULONG_PTR)baseAddress);

    // Make sure that the page containing the PE header for the managed asseblies is in the dump
    // especially on MacOS where they are added artificially.
    MemoryRegion header(flags, start, start + PAGE_SIZE);
    InsertMemoryRegion(header);

    // Add or change the module mapping for this PE image. The managed assembly images may already
    // be in the module mappings list but they may not have the full assembly name (like in .NET 2.0
    // they have the name "/dev/zero"). On MacOS, the managed assembly modules have not been added.
    const auto& found = m_moduleMappings.find(header);
    if (found == m_moduleMappings.end())
    {
        // On MacOS the assemblies are always added.
        MemoryRegion newRegion(flags, start, end, 0, name);
        m_moduleMappings.insert(newRegion);
        m_cbModuleMappings += newRegion.Size();

        if (g_diagnostics) {
            newRegion.Trace("MODULE: ADD ");
        }
    }
    else if (found->FileName().compare(name) != 0)
    {
        // Create the new memory region with the managed assembly name.
        MemoryRegion newRegion(*found, name);

        // Remove and cleanup the old one
        m_moduleMappings.erase(found);
        m_cbModuleMappings -= found->Size();

        // Add the new memory region.
        m_moduleMappings.insert(newRegion);
        m_cbModuleMappings += newRegion.Size();

        if (g_diagnostics) {
            newRegion.Trace("MODULE: REPLACE ");
        }
    }
}

//
// Returns the module base address for the IP or 0. Used by the thread unwind code.
//
uint64_t
CrashInfo::GetBaseAddressFromAddress(uint64_t address)
{
    MemoryRegion search(0, address, address, 0);
    const MemoryRegion* found = SearchMemoryRegions(m_moduleAddresses, search);
    if (found == nullptr) {
        return 0;
    }
    // The memory region Offset() is the base address of the module
    return found->Offset();
}

//
// Returns the module base address for the given module name or 0 if not found.
//
uint64_t
CrashInfo::GetBaseAddressFromName(const char* moduleName)
{
    for (const ModuleInfo* moduleInfo : m_moduleInfos)
    {
        std::string name = GetFileName(moduleInfo->ModuleName());
#ifdef __APPLE__
        // Module names are case insensitive on MacOS
        if (strcasecmp(name.c_str(), moduleName) == 0)
#else
        if (name.compare(moduleName) == 0)
#endif
        {
            return moduleInfo->BaseAddress();
        }
    }
    return 0;
}

//
// Return the module info for the base address
//
ModuleInfo*
CrashInfo::GetModuleInfoFromBaseAddress(uint64_t baseAddress)
{
    ModuleInfo search(baseAddress);
    const auto& found = m_moduleInfos.find(&search);
    if (found != m_moduleInfos.end())
    {
        return *found;
    }
    return nullptr;
}

//
// Adds module address range for IP lookup
//
void
CrashInfo::AddModuleAddressRange(uint64_t startAddress, uint64_t endAddress, uint64_t baseAddress)
{
    // Add module segment to base address lookup
    MemoryRegion region(0, startAddress, endAddress, baseAddress);
    m_moduleAddresses.insert(region);
}

//
// Adds module info (baseAddress, module name, etc)
//
void
CrashInfo::AddModuleInfo(bool isManaged, uint64_t baseAddress, IXCLRDataModule* pClrDataModule, const std::string& moduleName)
{
    ModuleInfo moduleInfo(baseAddress);
    const auto& found = m_moduleInfos.find(&moduleInfo);
    if (found == m_moduleInfos.end())
    {
        uint32_t timeStamp = 0;
        uint32_t imageSize = 0;
        bool isMainModule = false;
        GUID mvid;
        if (isManaged)
        {
            IMAGE_DOS_HEADER dosHeader;
            if (ReadMemory((void*)baseAddress, &dosHeader, sizeof(dosHeader)))
            {
                WORD magic;
                if (ReadMemory((void*)(baseAddress + dosHeader.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader.Magic)), &magic, sizeof(magic)))
                {
                    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
                    {
                        IMAGE_NT_HEADERS32 header;
                        if (ReadMemory((void*)(baseAddress + dosHeader.e_lfanew), &header, sizeof(header)))
                        {
                            imageSize = header.OptionalHeader.SizeOfImage;
                            timeStamp = header.FileHeader.TimeDateStamp;
                        }
                    }
                    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                    {
                        IMAGE_NT_HEADERS64 header;
                        if (ReadMemory((void*)(baseAddress + dosHeader.e_lfanew), &header, sizeof(header)))
                        {
                            imageSize = header.OptionalHeader.SizeOfImage;
                            timeStamp = header.FileHeader.TimeDateStamp;
                        }
                    }
                }
            }
            if (pClrDataModule != nullptr)
            {
                ULONG32 flags = 0;
                pClrDataModule->GetFlags(&flags);
                isMainModule = (flags & CLRDATA_MODULE_IS_MAIN_MODULE) != 0;
                pClrDataModule->GetVersionId(&mvid);
            }
            TRACE("MODULE: timestamp %08x size %08x %s %s%s\n", timeStamp, imageSize, FormatGuid(&mvid).c_str(), isMainModule ? "*" : "", moduleName.c_str());
        }
        ModuleInfo* moduleInfo = new ModuleInfo(isManaged, baseAddress, timeStamp, imageSize, &mvid, moduleName);
        if (isMainModule) {
            m_mainModule = moduleInfo;
        }
        m_moduleInfos.insert(moduleInfo);
    }
}

//
// ReadMemory from target and add to memory regions list
//
bool
CrashInfo::ReadMemory(void* address, void* buffer, size_t size)
{
    size_t read = 0;
    if (!ReadProcessMemory(address, buffer, size, &read))
    {
        return false;
    }
    assert(read == size);
    InsertMemoryRegion(reinterpret_cast<uint64_t>(address), read);
    return true;
}

//
// Add this memory chunk to the list of regions to be written to the core dump. Returns the number of pages actually added.
//
int
CrashInfo::InsertMemoryRegion(uint64_t address, size_t size)
{
    assert(size < UINT_MAX);

    // Round to page boundary
    uint64_t start = address & PAGE_MASK;
    assert(start > 0);

    // Round up to page boundary
    uint64_t end = ((address + size) + (PAGE_SIZE - 1)) & PAGE_MASK;
    assert(end > 0);

    return InsertMemoryRegion(MemoryRegion(GetMemoryRegionFlags(start), start, end));
}

//
// Add a memory region to the list. Returns the number of pages actually added.
//
int
CrashInfo::InsertMemoryRegion(const MemoryRegion& region)
{
    // First check if the full memory region can be added without conflicts and is fully valid.
    const auto& found = m_memoryRegions.find(region);
    if (found == m_memoryRegions.end())
    {
        // If the region is valid, add the full memory region
        if (ValidRegion(region))
        {
            m_memoryRegions.insert(region);
            return region.SizeInPages();
        }
    }
    else
    {
        // If the memory region is wholly contained in region found
        if (found->Contains(region))
        {
            return 0;
        }
    }
    // Either part of the region was invalid, part of it hasn't been added or the backed
    // by memory state is different.
    uint64_t start = region.StartAddress();

    // The region overlaps/conflicts with one already in the set so add one page at a
    // time to avoid the overlapping pages.
    uint64_t numberPages = region.SizeInPages();
    int pagesAdded = 0;

    for (size_t p = 0; p < numberPages; p++, start += PAGE_SIZE)
    {
        MemoryRegion memoryRegionPage(region.Flags(), start, start + PAGE_SIZE);

        const auto& found = m_memoryRegions.find(memoryRegionPage);
        if (found == m_memoryRegions.end())
        {
            // All the single pages added here will be combined in CombineMemoryRegions()
            if (ValidRegion(memoryRegionPage))
            {
                m_memoryRegions.insert(memoryRegionPage);
                pagesAdded++;
            }
        }
    }

    return pagesAdded;
}

//
// Validates a memory region
//
bool
CrashInfo::ValidRegion(const MemoryRegion& region)
{
    uint64_t start = region.StartAddress();
    uint64_t numberPages = region.SizeInPages();
    for (size_t p = 0; p < numberPages; p++, start += PAGE_SIZE)
    {
        BYTE buffer[1];
        size_t read;

        if (!ReadProcessMemory((void*)start, buffer, 1, &read))
        {
            return false;
        }
    }
    return true;
}

//
// Combine any adjacent memory regions into one
//
void
CrashInfo::CombineMemoryRegions()
{
    TRACE("CombineMemoryRegions: STARTED\n");
    assert(!m_memoryRegions.empty());
    std::set<MemoryRegion> memoryRegionsNew;

    // MEMORY_REGION_FLAG_SHARED and MEMORY_REGION_FLAG_PRIVATE are internal flags that
    // don't affect the core dump so ignore them when comparing the flags.
    uint32_t flags = m_memoryRegions.begin()->Flags() & MEMORY_REGION_FLAG_PERMISSIONS_MASK;
    uint64_t start = m_memoryRegions.begin()->StartAddress();
    uint64_t end = start;

    for (const MemoryRegion& region : m_memoryRegions)
    {
        // To combine a region it needs to be contiguous, same permissions and memory backed flag.
        if ((end == region.StartAddress()) &&
            (flags == (region.Flags() & MEMORY_REGION_FLAG_PERMISSIONS_MASK)))
        {
            end = region.EndAddress();
        }
        else
        {
            MemoryRegion memoryRegion(flags, start, end);
            assert(memoryRegionsNew.find(memoryRegion) == memoryRegionsNew.end());
            memoryRegionsNew.insert(memoryRegion);

            flags = region.Flags() & MEMORY_REGION_FLAG_PERMISSIONS_MASK;
            start = region.StartAddress();
            end = region.EndAddress();
        }
    }

    assert(start != end);
    MemoryRegion memoryRegion(flags, start, end);
    assert(memoryRegionsNew.find(memoryRegion) == memoryRegionsNew.end());
    memoryRegionsNew.insert(memoryRegion);

    m_memoryRegions = memoryRegionsNew;

    TRACE("CombineMemoryRegions: FINISHED\n");

    if (g_diagnosticsVerbose)
    {
        TRACE("Final Memory Regions:\n");
        for (const MemoryRegion& region : m_memoryRegions)
        {
            region.Trace();
        }
    }
}

//
// Searches for a memory region given an address.
//
const MemoryRegion*
CrashInfo::SearchMemoryRegions(const std::set<MemoryRegion>& regions, const MemoryRegion& search)
{
    std::set<MemoryRegion>::iterator found = regions.find(search);
    for (; found != regions.end(); found++)
    {
        if (search.StartAddress() >= found->StartAddress() && search.StartAddress() < found->EndAddress())
        {
            return &*found;
        }
    }
    return nullptr;
}

//
// Lookup a symbol in a module. The caller needs to call "free()" on symbol returned.
//
const char*
ModuleInfo::GetSymbolName(uint64_t address)
{
    LoadModule();

    if (m_localBaseAddress != 0)
    {
        uint64_t localAddress = m_localBaseAddress + (address - m_baseAddress);
        Dl_info info;
        if (dladdr((void*)localAddress, &info) != 0)
        {
            if (info.dli_sname != nullptr)
            {
                int status = -1;
                char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, 0, &status);
                return status == 0 ? demangled : strdup(info.dli_sname);
            }
        }
    }
    return nullptr;
}

//
// Returns just the file name portion of a file path
//
const std::string
GetFileName(const std::string& fileName)
{
    size_t last = fileName.rfind(DIRECTORY_SEPARATOR_STR_A);
    if (last != std::string::npos) {
        last++;
    }
    else {
        last = 0;
    }
    return fileName.substr(last);
}

//
// Returns just the directory portion of a path or empty if none
//
const std::string
GetDirectory(const std::string& fileName)
{
    size_t last = fileName.rfind(DIRECTORY_SEPARATOR_STR_A);
    if (last != std::string::npos) {
        last++;
    }
    else {
        last = 0;
    }
    return fileName.substr(0, last);
}

//
// Formats a std::string with printf syntax. The final formatted string is limited
// to MAX_LONGPATH (1024) chars. Returns an empty string on any error.
//
std::string
FormatString(const char* format, ...)
{
    ArrayHolder<char> buffer = new char[MAX_LONGPATH + 1];
    va_list args;
    va_start(args, format);
    int result = vsprintf_s(buffer, MAX_LONGPATH, format, args);
    va_end(args);
    return result > 0 ? std::string(buffer) : std::string();
}

//
// Format a guid
//
std::string
FormatGuid(const GUID* guid)
{
    uint8_t* bytes = (uint8_t*)guid;
    return FormatString("%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        bytes[3], bytes[2], bytes[1], bytes[0], bytes[5], bytes[4], bytes[7], bytes[6], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}
