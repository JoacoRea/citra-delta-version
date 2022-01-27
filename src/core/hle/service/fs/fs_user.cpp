// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include "common/assert.h"
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/seed_db.h"
#include "core/hle/ipc.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/server_session.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/fs/fs_user.h"
#include "core/settings.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace FS_User

using Kernel::ClientSession;
using Kernel::ServerSession;

namespace Service::FS {

void FS_USER::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x0801, 0, 2);
    u32 pid = rp.PopPID();

    ClientSlot* slot = GetSessionData(ctx.Session());
    slot->program_id = system.Kernel().GetProcessById(pid)->codeset->program_id;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
}

void FS_USER::OpenFile(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x0802, 7, 2);
    rp.Skip(1, false); // Transaction.

    ArchiveHandle archive_handle = rp.Pop<u64>();
    auto filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 filename_size = rp.Pop<u32>();
    FileSys::Mode mode{rp.Pop<u32>()};
    u32 attributes = rp.Pop<u32>(); // TODO(Link Mauve): do something with those attributes.
    std::vector<u8> filename = rp.PopStaticBuffer();
    ASSERT(filename.size() == filename_size);
    FileSys::Path file_path(filename_type, filename);

    LOG_DEBUG(Service_FS, "FS_USER OpenFile path={}, mode={} attrs={}", file_path.DebugStr(), mode.hex, attributes);

    const auto [file_res, open_timeout_ns] =
        archives.OpenFileFromArchive(archive_handle, file_path, mode);
    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(file_res.Code());
    if (file_res.Succeeded()) {
        std::shared_ptr<File> file = *file_res;
        rb.PushMoveObjects(file->Connect());
    } else {
        rb.PushMoveObjects<Kernel::Object>(nullptr);
        LOG_ERROR(Service_FS, "failed to get a handle for file {}", file_path.DebugStr());
    }

    ctx.SleepClientThread("fs_user::open", open_timeout_ns, nullptr);
}

void FS_USER::OpenFileDirectly(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x803, 8, 4);
    rp.Skip(1, false); // Transaction

    auto archive_id = rp.PopEnum<FS::ArchiveIdCode>();
    auto archivename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 archivename_size = rp.Pop<u32>();
    auto filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 filename_size = rp.Pop<u32>();
    FileSys::Mode mode{rp.Pop<u32>()};
    u32 attributes = rp.Pop<u32>(); // TODO(Link Mauve): do something with those attributes.
    std::vector<u8> archivename = rp.PopStaticBuffer();
    std::vector<u8> filename = rp.PopStaticBuffer();
    ASSERT(archivename.size() == archivename_size);
    ASSERT(filename.size() == filename_size);
    FileSys::Path archive_path(archivename_type, archivename);
    FileSys::Path file_path(filename_type, filename);

    LOG_DEBUG(Service_FS, "archive_id=0x{:08X} archive_path={} file_path={}, mode={} attributes={}",
              static_cast<u32>(archive_id), archive_path.DebugStr(), file_path.DebugStr(), mode.hex,
              attributes);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);

    ClientSlot* slot = GetSessionData(ctx.Session());

    ResultVal<ArchiveHandle> archive_handle =
        archives.OpenArchive(archive_id, archive_path, slot->program_id);
    if (archive_handle.Failed()) {
        LOG_ERROR(Service_FS,
                  "Failed to get a handle for archive archive_id=0x{:08X} archive_path={}",
                  static_cast<u32>(archive_id), archive_path.DebugStr());
        rb.Push(archive_handle.Code());
        rb.PushMoveObjects<Kernel::Object>(nullptr);
        return;
    }
    SCOPE_EXIT({ archives.CloseArchive(*archive_handle); });

    const auto [file_res, open_timeout_ns] =
        archives.OpenFileFromArchive(*archive_handle, file_path, mode);
    rb.Push(file_res.Code());
    if (file_res.Succeeded()) {
        std::shared_ptr<File> file = *file_res;
        rb.PushMoveObjects(file->Connect());
    } else {
        rb.PushMoveObjects<Kernel::Object>(nullptr);
        LOG_ERROR(Service_FS, "failed to get a handle for file {} mode={} attributes={}",
                  file_path.DebugStr(), mode.hex, attributes);
    }

    ctx.SleepClientThread("fs_user::open_directly", open_timeout_ns, nullptr);
}

void FS_USER::DeleteFile(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x804, 5, 2);
    rp.Skip(1, false); // TransactionId
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    auto filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 filename_size = rp.Pop<u32>();
    std::vector<u8> filename = rp.PopStaticBuffer();
    ASSERT(filename.size() == filename_size);

    FileSys::Path file_path(filename_type, filename);

    LOG_DEBUG(Service_FS, "FS_USER DeleteFile type={} size={} data={}", static_cast<u32>(filename_type), filename_size,
              file_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteFileFromArchive(archive_handle, file_path));
}

void FS_USER::RenameFile(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x805, 9, 4);
    rp.Skip(1, false); // TransactionId

    ArchiveHandle src_archive_handle = rp.PopRaw<ArchiveHandle>();
    auto src_filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 src_filename_size = rp.Pop<u32>();
    ArchiveHandle dest_archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dest_filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dest_filename_size = rp.Pop<u32>();
    std::vector<u8> src_filename = rp.PopStaticBuffer();
    std::vector<u8> dest_filename = rp.PopStaticBuffer();
    ASSERT(src_filename.size() == src_filename_size);
    ASSERT(dest_filename.size() == dest_filename_size);

    FileSys::Path src_file_path(src_filename_type, src_filename);
    FileSys::Path dest_file_path(dest_filename_type, dest_filename);

    LOG_DEBUG(Service_FS,
              "src_type={} src_size={} src_data={} dest_type={} dest_size={} dest_data={}",
              static_cast<u32>(src_filename_type), src_filename_size, src_file_path.DebugStr(),
              static_cast<u32>(dest_filename_type), dest_filename_size, dest_file_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.RenameFileBetweenArchives(src_archive_handle, src_file_path,
                                               dest_archive_handle, dest_file_path));
}

void FS_USER::DeleteDirectory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x806, 5, 2);

    rp.Skip(1, false); // TransactionId
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dirname_size = rp.Pop<u32>();
    std::vector<u8> dirname = rp.PopStaticBuffer();
    ASSERT(dirname.size() == dirname_size);

    FileSys::Path dir_path(dirname_type, dirname);

    LOG_DEBUG(Service_FS, "type={} size={} data={}", static_cast<u32>(dirname_type), dirname_size,
              dir_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteDirectoryFromArchive(archive_handle, dir_path));
}

void FS_USER::DeleteDirectoryRecursively(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x807, 5, 2);

    rp.Skip(1, false); // TransactionId
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dirname_size = rp.Pop<u32>();
    std::vector<u8> dirname = rp.PopStaticBuffer();
    ASSERT(dirname.size() == dirname_size);

    FileSys::Path dir_path(dirname_type, dirname);

    LOG_DEBUG(Service_FS, "type={} size={} data={}", static_cast<u32>(dirname_type), dirname_size,
              dir_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteDirectoryRecursivelyFromArchive(archive_handle, dir_path));
}

void FS_USER::CreateFile(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x808, 8, 2);

    rp.Skip(1, false); // TransactionId
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    auto filename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 filename_size = rp.Pop<u32>();
    u32 attributes = rp.Pop<u32>();
    u64 file_size = rp.Pop<u64>();
    std::vector<u8> filename = rp.PopStaticBuffer();
    ASSERT(filename.size() == filename_size);

    FileSys::Path file_path(filename_type, filename);

    LOG_DEBUG(Service_FS, "FS_USER CreateFile type={} attributes={} size={:x} data={}",
              static_cast<u32>(filename_type), attributes, file_size, file_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.CreateFileInArchive(archive_handle, file_path, file_size));
}

void FS_USER::CreateDirectory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x809, 6, 2);
    rp.Skip(1, false); // TransactionId
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dirname_size = rp.Pop<u32>();
    u32 attributes = rp.Pop<u32>();
    std::vector<u8> dirname = rp.PopStaticBuffer();
    ASSERT(dirname.size() == dirname_size);
    FileSys::Path dir_path(dirname_type, dirname);

    LOG_DEBUG(Service_FS, "FS_USER CreateDirectory type={} size={} data={}", static_cast<u32>(dirname_type), dirname_size,
              dir_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.CreateDirectoryFromArchive(archive_handle, dir_path));
}

void FS_USER::RenameDirectory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80A, 9, 4);
    rp.Skip(1, false); // TransactionId
    ArchiveHandle src_archive_handle = rp.PopRaw<ArchiveHandle>();
    auto src_dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 src_dirname_size = rp.Pop<u32>();
    ArchiveHandle dest_archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dest_dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dest_dirname_size = rp.Pop<u32>();
    std::vector<u8> src_dirname = rp.PopStaticBuffer();
    std::vector<u8> dest_dirname = rp.PopStaticBuffer();
    ASSERT(src_dirname.size() == src_dirname_size);
    ASSERT(dest_dirname.size() == dest_dirname_size);

    FileSys::Path src_dir_path(src_dirname_type, src_dirname);
    FileSys::Path dest_dir_path(dest_dirname_type, dest_dirname);

    LOG_DEBUG(Service_FS,
              "src_type={} src_size={} src_data={} dest_type={} dest_size={} dest_data={}",
              static_cast<u32>(src_dirname_type), src_dirname_size, src_dir_path.DebugStr(),
              static_cast<u32>(dest_dirname_type), dest_dirname_size, dest_dir_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.RenameDirectoryBetweenArchives(src_archive_handle, src_dir_path,
                                                    dest_archive_handle, dest_dir_path));
}

void FS_USER::OpenDirectory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80B, 4, 2);
    auto archive_handle = rp.PopRaw<ArchiveHandle>();
    auto dirname_type = rp.PopEnum<FileSys::LowPathType>();
    u32 dirname_size = rp.Pop<u32>();
    std::vector<u8> dirname = rp.PopStaticBuffer();
    ASSERT(dirname.size() == dirname_size);

    FileSys::Path dir_path(dirname_type, dirname);

    LOG_DEBUG(Service_FS, "FS_USER OpenDirectory type={} size={} data={}", static_cast<u32>(dirname_type), dirname_size,
              dir_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    ResultVal<std::shared_ptr<Directory>> dir_res =
        archives.OpenDirectoryFromArchive(archive_handle, dir_path);
    rb.Push(dir_res.Code());
    if (dir_res.Succeeded()) {
        std::shared_ptr<Directory> directory = *dir_res;
        auto [server, client] = system.Kernel().CreateSessionPair(directory->GetName());
        directory->ClientConnected(server);
        rb.PushMoveObjects(client);
    } else {
        LOG_ERROR(Service_FS, "failed to get a handle for directory type={} size={} data={}",
                  static_cast<u32>(dirname_type), dirname_size, dir_path.DebugStr());
        rb.PushMoveObjects<Kernel::Object>(nullptr);
    }
}

void FS_USER::OpenArchive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80C, 3, 2);
    auto archive_id = rp.PopEnum<FS::ArchiveIdCode>();
    auto archivename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 archivename_size = rp.Pop<u32>();
    std::vector<u8> archivename = rp.PopStaticBuffer();
    ASSERT(archivename.size() == archivename_size);
    FileSys::Path archive_path(archivename_type, archivename);

    LOG_DEBUG(Service_FS, "FS_USER OpenArchive archive_id=0x{:08X} archive_path={}", static_cast<u32>(archive_id),
              archive_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    ClientSlot* slot = GetSessionData(ctx.Session());
    ResultVal<ArchiveHandle> handle =
        archives.OpenArchive(archive_id, archive_path, slot->program_id);
    rb.Push(handle.Code());
    if (handle.Succeeded()) {
        rb.PushRaw(*handle);
    } else {
        rb.Push<u64>(0);
        LOG_ERROR(Service_FS,
                  "failed to get a handle for archive archive_id=0x{:08X} archive_path={}",
                  static_cast<u32>(archive_id), archive_path.DebugStr());
    }
}

void FS_USER::ControlArchive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80D, 5, 4);
    auto archive_handle = rp.PopRaw<ArchiveHandle>();
    const u32 action = rp.Pop<u32>();
    const u32 input_size = rp.Pop<u32>();
    const u32 output_size = rp.Pop<u32>();
    auto& input_buffer = rp.PopMappedBuffer();
    auto& output_buffer = rp.PopMappedBuffer();

    ResultCode result{RESULT_SUCCESS};
    if (action == 0) {
        // Action 0 : Commits save data changes.
        if (!archives.CheckArchiveHandle(archive_handle)) {
            result = ResultCode(FileSys::ErrCodes::ArchiveNotMounted, ErrorModule::FS,
                                ErrorSummary::NotFound, ErrorLevel::Status);
        }
    } else if (action == 1) {
        // Action 1 : Retrieves a file's last-modified timestamp.
        u64 timestamp = 0;
        std::u16string u16_filename(input_size + 1, '\0');
        input_buffer.Read(&u16_filename[0], 0, input_size);
        const std::string filename = Common::UTF16ToUTF8(u16_filename);
        timestamp = FileUtil::GetFileModificationTimestamp(filename);
        if (timestamp > 0) {
            output_buffer.Write(&timestamp, 0, sizeof(timestamp));
        } else {
            result = ResultCode(ErrorDescription::NotFound, ErrorModule::FS,
                                ErrorSummary::NotFound, ErrorLevel::Permanent);
        }
    } else if (action == 30877) {
        // Calls FSPXI command 0x00560102
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 4);
    rb.Push(result);
    rb.PushMappedBuffer(input_buffer);
    rb.PushMappedBuffer(output_buffer);
    LOG_WARNING(Service_FS, "(STUBBED) FS_USER ControlArchive action={:#08}, called input_size={:#08}, output_size={:#08}",
                action, input_size, output_size);
}

void FS_USER::CloseArchive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80E, 2, 0);
    auto archive_handle = rp.PopRaw<ArchiveHandle>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.CloseArchive(archive_handle));

    LOG_DEBUG(Service_GSP, "FS_USER CloseArchive called");
}

void FS_USER::IsSdmcDetected(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x817, 0, 0);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(Settings::values.use_virtual_sd);
}

void FS_USER::IsSdmcWriteable(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x818, 0, 0);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    // If the SD isn't enabled, it can't be writeable...else, stubbed true
    rb.Push(Settings::values.use_virtual_sd);
    LOG_DEBUG(Service_FS, " (STUBBED) FS_USER IsSdmcWriteable");
}

void FS_USER::FormatSaveData(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_FS, "(STUBBED) FS_USER FormatSaveData");

    IPC::RequestParser rp(ctx, 0x84C, 9, 2);
    auto archive_id = rp.PopEnum<FS::ArchiveIdCode>();
    auto archivename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 archivename_size = rp.Pop<u32>();
    u32 block_size = rp.Pop<u32>();
    u32 number_directories = rp.Pop<u32>();
    u32 number_files = rp.Pop<u32>();
    u32 directory_buckets = rp.Pop<u32>();
    u32 file_buckets = rp.Pop<u32>();
    bool duplicate_data = rp.Pop<bool>();
    std::vector<u8> archivename = rp.PopStaticBuffer();
    ASSERT(archivename.size() == archivename_size);
    FileSys::Path archive_path(archivename_type, archivename);

    LOG_DEBUG(Service_FS, "archive_path={}", archive_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    if (archive_id != FS::ArchiveIdCode::SaveData) {
        LOG_ERROR(Service_FS, "tried to format an archive different than SaveData, {}",
                  static_cast<u32>(archive_id));
        rb.Push(FileSys::ERROR_INVALID_PATH);
        return;
    }

    if (archive_path.GetType() != FileSys::LowPathType::Empty) {
        // TODO(Subv): Implement formatting the SaveData of other games
        LOG_ERROR(Service_FS, "archive LowPath type other than empty is currently unsupported");
        rb.Push(UnimplementedFunction(ErrorModule::FS));
        return;
    }

    FileSys::ArchiveFormatInfo format_info;
    format_info.duplicate_data = duplicate_data;
    format_info.number_directories = number_directories;
    format_info.number_files = number_files;
    format_info.total_size = block_size * 512;

    ClientSlot* slot = GetSessionData(ctx.Session());
    rb.Push(archives.FormatArchive(ArchiveIdCode::SaveData, format_info, archive_path,
                                   slot->program_id));
}

void FS_USER::FormatThisUserSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80F, 6, 0);
    u32 block_size = rp.Pop<u32>();
    u32 number_directories = rp.Pop<u32>();
    u32 number_files = rp.Pop<u32>();
    u32 directory_buckets = rp.Pop<u32>();
    u32 file_buckets = rp.Pop<u32>();
    bool duplicate_data = rp.Pop<bool>();

    FileSys::ArchiveFormatInfo format_info;
    format_info.duplicate_data = duplicate_data;
    format_info.number_directories = number_directories;
    format_info.number_files = number_files;
    format_info.total_size = block_size * 512;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    ClientSlot* slot = GetSessionData(ctx.Session());
    rb.Push(archives.FormatArchive(ArchiveIdCode::SaveData, format_info, FileSys::Path(),
                                   slot->program_id));

    LOG_TRACE(Service_FS, "FS_USER FormatThisUserSaveData called");
}

void FS_USER::GetFreeBytes(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x812, 2, 0);
    ArchiveHandle archive_handle = rp.PopRaw<ArchiveHandle>();
    ResultVal<u64> bytes_res = archives.GetFreeBytesInArchive(archive_handle);

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    rb.Push(bytes_res.Code());
    if (bytes_res.Succeeded()) {
        rb.Push<u64>(bytes_res.Unwrap());
    } else {
        rb.Push<u64>(0);
    }
}

void FS_USER::GetSdmcArchiveResource(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x814, 0, 0);

    LOG_WARNING(Service_FS, "(STUBBED) GetSdmcArchiveResource called");

    auto resource = archives.GetArchiveResource(MediaType::SDMC);

    if (resource.Failed()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(resource.Code());
        return;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(5, 0);
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw(*resource);
}

void FS_USER::GetNandArchiveResource(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x815, 0, 0);

    LOG_WARNING(Service_FS, "(STUBBED) GetNandArchiveResource called");

    auto resource = archives.GetArchiveResource(MediaType::NAND);
    if (resource.Failed()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(resource.Code());
        return;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(5, 0);
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw(*resource);
}

void FS_USER::CreateExtSaveData(Kernel::HLERequestContext& ctx) {
    // TODO(Subv): Figure out the other parameters.
    IPC::RequestParser rp(ctx, 0x0851, 9, 2);
    auto media_type = static_cast<MediaType>(rp.Pop<u32>()); // the other bytes are unknown
    u32 save_low = rp.Pop<u32>();
    u32 save_high = rp.Pop<u32>();
    u32 unknown = rp.Pop<u32>();
    u32 directories = rp.Pop<u32>();
    u32 files = rp.Pop<u32>();
    u64 size_limit = rp.Pop<u64>();
    u32 icon_size = rp.Pop<u32>();
    auto icon_buffer = rp.PopMappedBuffer();

    std::vector<u8> icon(icon_size);
    icon_buffer.Read(icon.data(), 0, icon_size);

    FileSys::ArchiveFormatInfo format_info;
    format_info.number_directories = directories;
    format_info.number_files = files;
    format_info.duplicate_data = false;
    format_info.total_size = 0;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    ClientSlot* slot = GetSessionData(ctx.Session());
    rb.Push(archives.CreateExtSaveData(media_type, save_high, save_low, icon, format_info,
                                       slot->program_id));
    rb.PushMappedBuffer(icon_buffer);

    LOG_DEBUG(Service_FS,
              "CreateExtSaveData called, savedata_high={:08X} savedata_low={:08X} unknown={:08X} "
              "files={:08X} directories={:08X} size_limit={:016x} icon_size={:08X}, media_type: {}",
              save_high, save_low, unknown, directories, files, size_limit, icon_size, (u32)media_type);
}

void FS_USER::DeleteExtSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x852, 4, 0);
    auto media_type = static_cast<MediaType>(rp.Pop<u32>()); // the other bytes are unknown
    u32 save_low = rp.Pop<u32>();
    u32 save_high = rp.Pop<u32>();
    u32 unknown = rp.Pop<u32>(); // TODO(Subv): Figure out what this is

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteExtSaveData(media_type, save_high, save_low));

    LOG_DEBUG(Service_FS,
              "DeleteExtSaveData called, save_low={:08X} save_high={:08X} media_type={:08X} unknown={:08X}",
              save_low, save_high, static_cast<u32>(media_type), unknown);
}

void FS_USER::CardSlotIsInserted(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x821, 0, 0);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(false);
    LOG_DEBUG(Service_FS, "(STUBBED) FS_USER CardSlotIsInserted called");
}

void FS_USER::GetCardType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x813, 0, 0);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(false); // CTR card = 0, TWL card = 1
    LOG_DEBUG(Service_FS, "(STUBBED) FS_USER GetCardType called");
}

void FS_USER::DeleteSystemSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x857, 2, 0);
    u32 savedata_high = rp.Pop<u32>();
    u32 savedata_low = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteSystemSaveData(savedata_high, savedata_low));

    LOG_DEBUG(Service_FS, "(STUBBED) FS_USER DeleteSystemSaveData called");
}

void FS_USER::EnumerateExtSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x855, 4, 2);
    u32 buff_size = rp.Pop<u32>();
    auto media_type = rp.PopEnum<MediaType>();
    u32 unknown = rp.Pop<u32>();
    u32 shared = rp.Pop<u32>();
    auto& extdata_list = rp.PopMappedBuffer();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);

    std::string title_path;
    if (media_type == Service::FS::MediaType::NAND) {
        title_path = fmt::format("{}{}/extdata/",
                                 FileUtil::GetUserPath(FileUtil::UserPath::NANDDir), SYSTEM_ID);
    } else if (media_type == Service::FS::MediaType::SDMC) {
        title_path = fmt::format("{}Nintendo 3DS/{}/{}/extdata/",
                                 FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), SYSTEM_ID, SDCARD_ID);
    } else {
        rb.Push(RESULT_SUCCESS);
        rb.Push(0);
        return;
    }

    std::vector<u64_le> title_list;
    FileUtil::FSTEntry entries;
    constexpr std::size_t TITLE_ID_VALID_LENGTH = 16;
    FileUtil::ScanDirectoryTree(title_path, entries, 1);
    for (const FileUtil::FSTEntry& tid_high : entries.children) {
        for (const FileUtil::FSTEntry& tid_low : tid_high.children) {
            std::string tid_string = tid_high.virtualName + tid_low.virtualName;
            if (tid_string.length() == TITLE_ID_VALID_LENGTH) {
                std::string user_path = fmt::format("{}{}/{}/user/", title_path, tid_high.virtualName, tid_low.virtualName);
                if (FileUtil::Exists(user_path)) {
                    const u64 tid = std::stoull(tid_string, nullptr, 16);
                    title_list.push_back(tid);
                }
            }
        }
    }

    u32 count = std::min(title_list.size(), buff_size / sizeof(u64));
    extdata_list.Write(title_list.data(), 0, count * sizeof(u64));
    rb.Push(RESULT_SUCCESS);
    rb.Push(count);

    LOG_DEBUG(Service_FS, "FS_USER EnumerateExtSaveData buff_size={} media_type={:08X}, unk: {}, shared: {:08X}",
              buff_size, static_cast<u32>(media_type), unknown, shared);
}

void FS_USER::CreateSystemSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x856, 9, 0);
    u32 savedata_high = rp.Pop<u32>();
    u32 savedata_low = rp.Pop<u32>();
    u32 total_size = rp.Pop<u32>();
    u32 block_size = rp.Pop<u32>();
    u32 directories = rp.Pop<u32>();
    u32 files = rp.Pop<u32>();
    u32 directory_buckets = rp.Pop<u32>();
    u32 file_buckets = rp.Pop<u32>();
    bool duplicate = rp.Pop<bool>();

    LOG_WARNING(
        Service_FS,
        "(STUBBED) savedata_high={:08X} savedata_low={:08X} total_size={:08X}  block_size={:08X} "
        "directories={} files={} directory_buckets={} file_buckets={} duplicate={}",
        savedata_high, savedata_low, total_size, block_size, directories, files, directory_buckets,
        file_buckets, duplicate);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.CreateSystemSaveData(savedata_high, savedata_low));
}

void FS_USER::CreateLegacySystemSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x810, 8, 0);
    u32 savedata_id = rp.Pop<u32>();
    u32 total_size = rp.Pop<u32>();
    u32 block_size = rp.Pop<u32>();
    u32 directories = rp.Pop<u32>();
    u32 files = rp.Pop<u32>();
    u32 directory_buckets = rp.Pop<u32>();
    u32 file_buckets = rp.Pop<u32>();
    bool duplicate = rp.Pop<bool>();

    LOG_WARNING(Service_FS,
                "(STUBBED) savedata_id={:08X} total_size={:08X} block_size={:08X} directories={} "
                "files={} directory_buckets={} file_buckets={} duplicate={}",
                savedata_id, total_size, block_size, directories, files, directory_buckets,
                file_buckets, duplicate);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    // With this command, the SystemSaveData always has save_high = 0 (Always created in the NAND)
    rb.Push(archives.CreateSystemSaveData(0, savedata_id));
}

void FS_USER::InitializeWithSdkVersion(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x861, 1, 2);
    const u32 version = rp.Pop<u32>();
    u32 pid = rp.PopPID();

    ClientSlot* slot = GetSessionData(ctx.Session());
    slot->program_id = system.Kernel().GetProcessById(pid)->codeset->program_id;

    LOG_WARNING(Service_FS, "(STUBBED) FS_USER InitializeWithSdkVersion called, version: 0x{:08X}", version);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
}

void FS_USER::SetPriority(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x862, 1, 0);

    priority = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_DEBUG(Service_FS, "FS_USER SetPriority called priority=0x{:X}", priority);
}

void FS_USER::GetPriority(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x863, 0, 0);

    if (priority == -1) {
        LOG_INFO(Service_FS, "priority was not set, priority=0x{:X}", priority);
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(priority);

    LOG_DEBUG(Service_FS, "FS_USER GetPriority called priority=0x{:X}", priority);
}

void FS_USER::GetArchiveResource(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x849, 1, 0);
    auto media_type = rp.PopEnum<MediaType>();

    LOG_WARNING(Service_FS, "(STUBBED) FS_USER GetArchiveResource called Media type=0x{:08X}", static_cast<u32>(media_type));

    auto resource = archives.GetArchiveResource(media_type);
    if (resource.Failed()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(resource.Code());
        return;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(5, 0);
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw(*resource);
}

void FS_USER::GetFormatInfo(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x845, 3, 2);
    auto archive_id = rp.PopEnum<FS::ArchiveIdCode>();
    auto archivename_type = rp.PopEnum<FileSys::LowPathType>();
    u32 archivename_size = rp.Pop<u32>();
    std::vector<u8> archivename = rp.PopStaticBuffer();
    ASSERT(archivename.size() == archivename_size);

    FileSys::Path archive_path(archivename_type, archivename);

    LOG_DEBUG(Service_FS, "FS_USER GetFormatInfo archive_path={}", archive_path.DebugStr());

    IPC::RequestBuilder rb = rp.MakeBuilder(5, 0);
    ClientSlot* slot = GetSessionData(ctx.Session());
    auto format_info = archives.GetArchiveFormatInfo(archive_id, archive_path, slot->program_id);
    rb.Push(format_info.Code());
    if (format_info.Failed()) {
        LOG_ERROR(Service_FS, "Failed to retrieve the format info");
        rb.Skip(4, true);
        return;
    }

    rb.Push<u32>(format_info->total_size);
    rb.Push<u32>(format_info->number_directories);
    rb.Push<u32>(format_info->number_files);
    rb.Push<bool>(format_info->duplicate_data != 0);
}

void FS_USER::GetProgramLaunchInfo(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x82F, 1, 0);

    u32 process_id = rp.Pop<u32>();

    LOG_DEBUG(Service_FS, "FS_USER GetProgramLaunchInfo process_id={}", process_id);

    auto program_info = program_info_map.find(process_id);

    IPC::RequestBuilder rb = rp.MakeBuilder(5, 0);

    if (program_info == program_info_map.end()) {
        // Note: In this case, the rest of the parameters are not changed but the command header
        // remains the same.
        rb.Push(ResultCode(FileSys::ErrCodes::ArchiveNotMounted, ErrorModule::FS,
                           ErrorSummary::NotFound, ErrorLevel::Status));
        rb.Skip(4, false);
        return;
    }

    rb.Push(RESULT_SUCCESS);
    rb.Push(program_info->second.program_id);
    rb.Push(static_cast<u8>(program_info->second.media_type));

    // TODO(Subv): Find out what this value means.
    rb.Push<u32>(0);
}

void FS_USER::ObsoletedCreateExtSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x830, 6, 2);
    auto media_type = rp.PopEnum<MediaType>();
    u32 save_low = rp.Pop<u32>();
    u32 save_high = rp.Pop<u32>();
    u32 icon_size = rp.Pop<u32>();
    u32 directories = rp.Pop<u32>();
    u32 files = rp.Pop<u32>();
    auto icon_buffer = rp.PopMappedBuffer();

    std::vector<u8> icon(icon_size);
    icon_buffer.Read(icon.data(), 0, icon_size);

    FileSys::ArchiveFormatInfo format_info;
    format_info.number_directories = directories;
    format_info.number_files = files;
    format_info.duplicate_data = false;
    format_info.total_size = 0;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    ClientSlot* slot = GetSessionData(ctx.Session());
    rb.Push(archives.CreateExtSaveData(media_type, save_high, save_low, icon, format_info,
                                       slot->program_id));
    rb.PushMappedBuffer(icon_buffer);

    LOG_DEBUG(Service_FS,
              "called, savedata_high={:08X} savedata_low={:08X} "
              "icon_size={:08X} files={:08X} directories={:08X}",
              save_high, save_low, icon_size, directories, files);
}

void FS_USER::ObsoletedDeleteExtSaveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x835, 2, 0);
    auto media_type = rp.PopEnum<MediaType>();
    u32 save_low = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(archives.DeleteExtSaveData(media_type, 0, save_low));

    LOG_DEBUG(Service_FS, "called, save_low={:08X} media_type={:08X}", save_low,
              static_cast<u32>(media_type));
}

void FS_USER::GetSpecialContentIndex(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x83A, 4, 0);
    const auto media_type = rp.PopEnum<MediaType>();
    const u64 title_id = rp.Pop<u64>();
    const auto type = rp.PopEnum<SpecialContentType>();

    LOG_DEBUG(Service_FS, "called, media_type={:08X} type={:08X}, title_id={:016X}",
              static_cast<u32>(media_type), static_cast<u32>(type), title_id);

    ResultVal<u16> index;
    if (media_type == MediaType::GameCard) {
        index = GetSpecialContentIndexFromGameCard(title_id, type);
    } else {
        index = GetSpecialContentIndexFromTMD(media_type, title_id, type);
    }

    if (index.Succeeded()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
        rb.Push(RESULT_SUCCESS);
        rb.Push(index.Unwrap());
    } else {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(index.Code());
    }
}

void FS_USER::GetNumSeeds(Kernel::HLERequestContext& ctx) {
    IPC::RequestBuilder rb{ctx, 0x87D, 2, 0};
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(FileSys::GetSeedCount());
}

void FS_USER::AddSeed(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx, 0x87A, 6, 0};
    u64 title_id{rp.Pop<u64>()};
    FileSys::Seed::Data seed{rp.PopRaw<FileSys::Seed::Data>()};
    FileSys::AddSeed({title_id, seed, {}});
    IPC::RequestBuilder rb{rp.MakeBuilder(1, 0)};
    rb.Push(RESULT_SUCCESS);
}

void FS_USER::SetSaveDataSecureValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x865, 5, 0);
    u64 value = rp.Pop<u64>();
    u32 secure_value_slot = rp.Pop<u32>();
    u32 unique_id = rp.Pop<u32>();
    u8 title_variation = rp.Pop<u8>();

    // TODO: Generate and Save the Secure Value

    LOG_WARNING(Service_FS,
                "(STUBBED) called, value=0x{:016x} secure_value_slot=0x{:08X} "
                "unqiue_id=0x{:08X} title_variation=0x{:02X}",
                value, secure_value_slot, unique_id, title_variation);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    rb.Push(RESULT_SUCCESS);
}

void FS_USER::GetSaveDataSecureValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x866, 3, 0);

    u32 secure_value_slot = rp.Pop<u32>();
    u32 unique_id = rp.Pop<u32>();
    u8 title_variation = rp.Pop<u8>();

    LOG_WARNING(
        Service_FS,
        "(STUBBED) called secure_value_slot=0x{:08X} unqiue_id=0x{:08X} title_variation=0x{:02X}",
        secure_value_slot, unique_id, title_variation);

    IPC::RequestBuilder rb = rp.MakeBuilder(4, 0);

    rb.Push(RESULT_SUCCESS);

    // TODO: Implement Secure Value Lookup & Generation

    rb.Push<bool>(false); // indicates that the secure value doesn't exist
    rb.Push<u64>(0);      // the secure value
}

void FS_USER::Register(u32 process_id, u64 program_id, const std::string& filepath) {
    const MediaType media_type = GetMediaTypeFromPath(filepath);
    program_info_map.insert_or_assign(process_id, ProgramInfo{program_id, media_type});
    if (media_type == MediaType::GameCard) {
        current_gamecard_path = filepath;
    }
}

std::string FS_USER::GetCurrentGamecardPath() const {
    return current_gamecard_path;
}

ResultVal<u16> FS_USER::GetSpecialContentIndexFromGameCard(u64 title_id, SpecialContentType type) {
    // TODO(B3N30) check if on real 3DS NCSD is checked if partition exists

    if (type > SpecialContentType::DLPChild) {
        // Maybe type 4 is New 3DS update/partition 6 but this needs more research
        // TODO(B3N30): Find correct result code
        return ResultCode(-1);
    }

    switch (type) {
    case SpecialContentType::Update:
        return MakeResult(static_cast<u16>(NCSDContentIndex::Update));
    case SpecialContentType::Manual:
        return MakeResult(static_cast<u16>(NCSDContentIndex::Manual));
    case SpecialContentType::DLPChild:
        return MakeResult(static_cast<u16>(NCSDContentIndex::DLP));
    default:
        ASSERT(false);
    }
}

ResultVal<u16> FS_USER::GetSpecialContentIndexFromTMD(MediaType media_type, u64 title_id,
                                                      SpecialContentType type) {
    if (type > SpecialContentType::DLPChild) {
        // TODO(B3N30): Find correct result code
        return ResultCode(-1);
    }

    std::string tmd_path = AM::GetTitleMetadataPath(media_type, title_id);

    FileSys::TitleMetadata tmd;
    if (tmd.Load(tmd_path) != Loader::ResultStatus::Success || type == SpecialContentType::Update) {
        // TODO(B3N30): Find correct result code
        return ResultCode(-1);
    }

    // TODO(B3N30): Does real 3DS check if content exists in TMD?

    switch (type) {
    case SpecialContentType::Manual:
        return MakeResult(static_cast<u16>(FileSys::TMDContentIndex::Manual));
    case SpecialContentType::DLPChild:
        return MakeResult(static_cast<u16>(FileSys::TMDContentIndex::DLP));
    default:
        ASSERT(false);
    }
}

FS_USER::FS_USER(Core::System& system)
    : ServiceFramework("fs:USER", 30), system(system), archives(system.ArchiveManager()) {
    static const FunctionInfo functions[] = {
        {0x000100C6, nullptr, "Dummy1"},
        {0x040100C4, nullptr, "Control"},
        {0x08010002, &FS_USER::Initialize, "Initialize"},
        {0x080201C2, &FS_USER::OpenFile, "OpenFile"},
        {0x08030204, &FS_USER::OpenFileDirectly, "OpenFileDirectly"},
        {0x08040142, &FS_USER::DeleteFile, "DeleteFile"},
        {0x08050244, &FS_USER::RenameFile, "RenameFile"},
        {0x08060142, &FS_USER::DeleteDirectory, "DeleteDirectory"},
        {0x08070142, &FS_USER::DeleteDirectoryRecursively, "DeleteDirectoryRecursively"},
        {0x08080202, &FS_USER::CreateFile, "CreateFile"},
        {0x08090182, &FS_USER::CreateDirectory, "CreateDirectory"},
        {0x080A0244, &FS_USER::RenameDirectory, "RenameDirectory"},
        {0x080B0102, &FS_USER::OpenDirectory, "OpenDirectory"},
        {0x080C00C2, &FS_USER::OpenArchive, "OpenArchive"},
        {0x080D0144, &FS_USER::ControlArchive, "ControlArchive"},
        {0x080E0080, &FS_USER::CloseArchive, "CloseArchive"},
        {0x080F0180, &FS_USER::FormatThisUserSaveData, "FormatThisUserSaveData"},
        {0x08100200, &FS_USER::CreateLegacySystemSaveData, "CreateLegacySystemSaveData"},
        {0x08110040, nullptr, "DeleteSystemSaveData"},
        {0x08120080, &FS_USER::GetFreeBytes, "GetFreeBytes"},
        {0x08130000, &FS_USER::GetCardType, "GetCardType"},
        {0x08140000, &FS_USER::GetSdmcArchiveResource, "GetSdmcArchiveResource"},
        {0x08150000, &FS_USER::GetNandArchiveResource, "GetNandArchiveResource"},
        {0x08160000, nullptr, "GetSdmcFatfsError"},
        {0x08170000, &FS_USER::IsSdmcDetected, "IsSdmcDetected"},
        {0x08180000, &FS_USER::IsSdmcWriteable, "IsSdmcWritable"},
        {0x08190042, nullptr, "GetSdmcCid"},
        {0x081A0042, nullptr, "GetNandCid"},
        {0x081B0000, nullptr, "GetSdmcSpeedInfo"},
        {0x081C0000, nullptr, "GetNandSpeedInfo"},
        {0x081D0042, nullptr, "GetSdmcLog"},
        {0x081E0042, nullptr, "GetNandLog"},
        {0x081F0000, nullptr, "ClearSdmcLog"},
        {0x08200000, nullptr, "ClearNandLog"},
        {0x08210000, &FS_USER::CardSlotIsInserted, "CardSlotIsInserted"},
        {0x08220000, nullptr, "CardSlotPowerOn"},
        {0x08230000, nullptr, "CardSlotPowerOff"},
        {0x08240000, nullptr, "CardSlotGetCardIFPowerStatus"},
        {0x08250040, nullptr, "CardNorDirectCommand"},
        {0x08260080, nullptr, "CardNorDirectCommandWithAddress"},
        {0x08270082, nullptr, "CardNorDirectRead"},
        {0x082800C2, nullptr, "CardNorDirectReadWithAddress"},
        {0x08290082, nullptr, "CardNorDirectWrite"},
        {0x082A00C2, nullptr, "CardNorDirectWriteWithAddress"},
        {0x082B00C2, nullptr, "CardNorDirectRead_4xIO"},
        {0x082C0082, nullptr, "CardNorDirectCpuWriteWithoutVerify"},
        {0x082D0040, nullptr, "CardNorDirectSectorEraseWithoutVerify"},
        {0x082E0040, nullptr, "GetProductInfo"},
        {0x082F0040, &FS_USER::GetProgramLaunchInfo, "GetProgramLaunchInfo"},
        {0x08300182, &FS_USER::ObsoletedCreateExtSaveData, "Obsoleted_3_0_CreateExtSaveData"},
        {0x08310180, nullptr, "CreateSharedExtSaveData"},
        {0x08320102, nullptr, "ReadExtSaveDataIcon"},
        {0x08330082, nullptr, "EnumerateExtSaveData"},
        {0x08340082, nullptr, "EnumerateSharedExtSaveData"},
        {0x08350080, &FS_USER::ObsoletedDeleteExtSaveData, "Obsoleted_3_0_DeleteExtSaveData"},
        {0x08360080, nullptr, "DeleteSharedExtSaveData"},
        {0x08370040, nullptr, "SetCardSpiBaudRate"},
        {0x08380040, nullptr, "SetCardSpiBusMode"},
        {0x08390000, nullptr, "SendInitializeInfoTo9"},
        {0x083A0100, &FS_USER::GetSpecialContentIndex, "GetSpecialContentIndex"},
        {0x083B00C2, nullptr, "GetLegacyRomHeader"},
        {0x083C00C2, nullptr, "GetLegacyBannerData"},
        {0x083D0100, nullptr, "CheckAuthorityToAccessExtSaveData"},
        {0x083E00C2, nullptr, "QueryTotalQuotaSize"},
        {0x083F00C0, nullptr, "GetExtDataBlockSize"},
        {0x08400040, nullptr, "AbnegateAccessRight"},
        {0x08410000, nullptr, "DeleteSdmcRoot"},
        {0x08420040, nullptr, "DeleteAllExtSaveDataOnNand"},
        {0x08430000, nullptr, "InitializeCtrFileSystem"},
        {0x08440000, nullptr, "CreateSeed"},
        {0x084500C2, &FS_USER::GetFormatInfo, "GetFormatInfo"},
        {0x08460102, nullptr, "GetLegacyRomHeader2"},
        {0x08470180, nullptr, "FormatCtrCardUserSaveData"},
        {0x08480042, nullptr, "GetSdmcCtrRootPath"},
        {0x08490040, &FS_USER::GetArchiveResource, "GetArchiveResource"},
        {0x084A0002, nullptr, "ExportIntegrityVerificationSeed"},
        {0x084B0002, nullptr, "ImportIntegrityVerificationSeed"},
        {0x084C0242, &FS_USER::FormatSaveData, "FormatSaveData"},
        {0x084D0102, nullptr, "GetLegacySubBannerData"},
        {0x084E0342, nullptr, "UpdateSha256Context"},
        {0x084F0102, nullptr, "ReadSpecialFile"},
        {0x08500040, nullptr, "GetSpecialFileSize"},
        {0x08510242, &FS_USER::CreateExtSaveData, "CreateExtSaveData"},
        {0x08520100, &FS_USER::DeleteExtSaveData, "DeleteExtSaveData"},
        {0x08530142, nullptr, "ReadExtSaveDataIcon"},
        {0x085400C0, nullptr, "GetExtDataBlockSize"},
        {0x08550102, &FS_USER::EnumerateExtSaveData, "EnumerateExtSaveData"},
        {0x08560240, &FS_USER::CreateSystemSaveData, "CreateSystemSaveData"},
        {0x08570080, &FS_USER::DeleteSystemSaveData, "DeleteSystemSaveData"},
        {0x08580000, nullptr, "StartDeviceMoveAsSource"},
        {0x08590200, nullptr, "StartDeviceMoveAsDestination"},
        {0x085A00C0, nullptr, "SetArchivePriority"},
        {0x085B0080, nullptr, "GetArchivePriority"},
        {0x085C00C0, nullptr, "SetCtrCardLatencyParameter"},
        {0x085D01C0, nullptr, "SetFsCompatibilityInfo"},
        {0x085E0040, nullptr, "ResetCardCompatibilityParameter"},
        {0x085F0040, nullptr, "SwitchCleanupInvalidSaveData"},
        {0x08600042, nullptr, "EnumerateSystemSaveData"},
        {0x08610042, &FS_USER::InitializeWithSdkVersion, "InitializeWithSdkVersion"},
        {0x08620040, &FS_USER::SetPriority, "SetPriority"},
        {0x08630000, &FS_USER::GetPriority, "GetPriority"},
        {0x08640000, nullptr, "GetNandInfo"},
        {0x08650140, &FS_USER::SetSaveDataSecureValue, "SetSaveDataSecureValue"},
        {0x086600C0, &FS_USER::GetSaveDataSecureValue, "GetSaveDataSecureValue"},
        {0x086700C4, nullptr, "ControlSecureSave"},
        {0x08680000, nullptr, "GetMediaType"},
        {0x08690000, nullptr, "GetNandEraseCount"},
        {0x086A0082, nullptr, "ReadNandReport"},
        {0x086B00C2, nullptr, "SetOtherSaveDataSecureValue"},
        {0x086C00C2, nullptr, "GetOtherSaveDataSecureValue"},
        {0x086E00C0, nullptr, "SetThisSaveDataSecureValue"},
        {0x086F0040, nullptr, "GetThisSaveDataSecureValue"},
        {0x087A0180, &FS_USER::AddSeed, "AddSeed"},
        {0x087D0000, &FS_USER::GetNumSeeds, "GetNumSeeds"},
        {0x088600C0, nullptr, "CheckUpdatedDat"},
    };
    RegisterHandlers(functions);
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<FS_USER>(system)->InstallAsService(service_manager);
}
} // namespace Service::FS
