#pragma once
#include "bitfield.h"
#include "progress_callback.h"
#include "types.h"
#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Common {
class Error;
}

class CDImage
{
public:
  enum class OpenFlags : uint8_t
  {
    None = 0,
    PreCache = (1 << 0), // Pre-cache image to RAM, if supported.
  };

  CDImage(OpenFlags open_flags);
  virtual ~CDImage();

  using LBA = uint32_t;

  static constexpr uint32_t RAW_SECTOR_SIZE = 2352, DATA_SECTOR_SIZE = 2048, SECTOR_SYNC_SIZE = 12, SECTOR_HEADER_SIZE = 4,
                       FRAMES_PER_SECOND = 75, // "sectors", or "timecode frames" (not "channel frames")
    SECONDS_PER_MINUTE = 60, FRAMES_PER_MINUTE = FRAMES_PER_SECOND * SECONDS_PER_MINUTE,
                       SUBCHANNEL_BYTES_PER_FRAME = 12, LEAD_OUT_SECTOR_COUNT = 6750;

  static constexpr uint8_t LEAD_OUT_TRACK_NUMBER = 0xAA;

  enum class ReadMode : uint32_t
  {
    DataOnly,  // 2048 bytes per sector.
    RawSector, // 2352 bytes per sector.
    RawNoSync, // 2340 bytes per sector.
  };

  enum class TrackMode : uint32_t
  {
    Audio,        // 2352 bytes per sector
    Mode1,        // 2048 bytes per sector
    Mode1Raw,     // 2352 bytes per sector
    Mode2,        // 2336 bytes per sector
    Mode2Form1,   // 2048 bytes per sector
    Mode2Form2,   // 2324 bytes per sector
    Mode2FormMix, // 2332 bytes per sector
    Mode2Raw      // 2352 bytes per sector
  };

  struct SectorHeader
  {
    uint8_t minute;
    uint8_t second;
    uint8_t frame;
    uint8_t sector_mode;
  };

  struct Position
  {
    uint8_t minute;
    uint8_t second;
    uint8_t frame;

    static constexpr Position FromBCD(uint8_t minute, uint8_t second, uint8_t frame)
    {
      return Position{PackedBCDToBinary(minute), PackedBCDToBinary(second), PackedBCDToBinary(frame)};
    }

    static constexpr Position FromLBA(LBA lba)
    {
      const uint8_t frame = static_cast<uint8_t>(lba % FRAMES_PER_SECOND);
      lba /= FRAMES_PER_SECOND;

      const uint8_t second = static_cast<uint8_t>(lba % SECONDS_PER_MINUTE);
      lba /= SECONDS_PER_MINUTE;

      const uint8_t minute = static_cast<uint8_t>(lba);

      return Position{minute, second, frame};
    }

    LBA ToLBA() const
    {
      return static_cast<uint32_t>(minute) * FRAMES_PER_MINUTE + static_cast<uint32_t>(second) * FRAMES_PER_SECOND + static_cast<uint32_t>(frame);
    }

    constexpr std::tuple<uint8_t, uint8_t, uint8_t> ToBCD() const
    {
      return std::make_tuple<uint8_t, uint8_t, uint8_t>(BinaryToBCD(minute), BinaryToBCD(second), BinaryToBCD(frame));
    }

    Position operator+(const Position& rhs) { return FromLBA(ToLBA() + rhs.ToLBA()); }
    Position& operator+=(const Position& pos)
    {
      *this = *this + pos;
      return *this;
    }

#define RELATIONAL_OPERATOR(op)                                                                                        \
  bool operator op(const Position& rhs) const                                                                          \
  {                                                                                                                    \
    return std::tie(minute, second, frame) op std::tie(rhs.minute, rhs.second, rhs.frame);                             \
  }

    RELATIONAL_OPERATOR(==);
    RELATIONAL_OPERATOR(!=);
    RELATIONAL_OPERATOR(<);
    RELATIONAL_OPERATOR(<=);
    RELATIONAL_OPERATOR(>);
    RELATIONAL_OPERATOR(>=);

#undef RELATIONAL_OPERATOR
  };

  union SubChannelQ
  {
    using Data = std::array<uint8_t, SUBCHANNEL_BYTES_PER_FRAME>;

    union Control
    {
      uint8_t bits;

      BitField<uint8_t, uint8_t, 0, 4> adr;
      BitField<uint8_t, bool, 4, 1> audio_preemphasis;
      BitField<uint8_t, bool, 5, 1> digital_copy_permitted;
      BitField<uint8_t, bool, 6, 1> data;
      BitField<uint8_t, bool, 7, 1> four_channel_audio;

      Control() = default;
      Control(const Control&) = default;

      Control& operator=(const Control& rhs)
      {
        bits = rhs.bits;
        return *this;
      }
    };

    struct
    {
      uint8_t control_bits;
      uint8_t track_number_bcd;
      uint8_t index_number_bcd;
      uint8_t relative_minute_bcd;
      uint8_t relative_second_bcd;
      uint8_t relative_frame_bcd;
      uint8_t reserved;
      uint8_t absolute_minute_bcd;
      uint8_t absolute_second_bcd;
      uint8_t absolute_frame_bcd;
      uint16_t crc;
    };

    Data data;

    static uint16_t ComputeCRC(const Data& data);

    Control GetControl() const { return Control{control_bits}; }
    bool IsData() const { return GetControl().data; }

    bool IsCRCValid() const;

    SubChannelQ& operator=(const SubChannelQ& q)
    {
      data = q.data;
      return *this;
    }
  };

  struct Track
  {
    uint32_t track_number;
    LBA start_lba;
    uint32_t first_index;
    uint32_t length;
    TrackMode mode;
    SubChannelQ::Control control;
  };

  struct Index
  {
    uint64_t file_offset;
    uint32_t file_index;
    uint32_t file_sector_size;
    LBA start_lba_on_disc;
    uint32_t track_number;
    uint32_t index_number;
    LBA start_lba_in_track;
    uint32_t length;
    TrackMode mode;
    SubChannelQ::Control control;
    bool is_pregap;
  };

  // Helper functions.
  static uint32_t GetBytesPerSector(TrackMode mode);

  // Opening disc image.
  static std::unique_ptr<CDImage> Open(const char* filename, OpenFlags open_flags, Common::Error* error);
  // A frontend's own discs (PSSwanStation: a disc in a USB drive). Asked first
  // by Open(): true when the path is the hook's, with *out set, or null and
  // the error set when it could not be opened.
  using OpenHook = bool (*)(const char* filename, std::unique_ptr<CDImage>* out, Common::Error* error);
  static void SetOpenHook(OpenHook hook);
  static std::unique_ptr<CDImage> OpenBinImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenCueSheetImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenCHDImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenEcmImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenMdsImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenPBPImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage> OpenM3uImage(const char* filename, OpenFlags open_flags, Common::Error* error);
  static std::unique_ptr<CDImage>
  CreateMemoryImage(CDImage* image, ProgressCallback* progress = ProgressCallback::NullProgressCallback);
  static std::unique_ptr<CDImage> OverlayPPFPatch(const char* filename, OpenFlags open_flags,
                                                  std::unique_ptr<CDImage> parent_image,
                                                  ProgressCallback* progress = ProgressCallback::NullProgressCallback);

  // Accessors.
  const std::string& GetFileName() const { return m_filename; }
  LBA GetPositionOnDisc() const { return m_position_on_disc; }
  LBA GetLBACount() const { return m_lba_count; }
  uint32_t GetTrackNumber() const { return m_current_index->track_number; }
  uint32_t GetTrackCount() const { return static_cast<uint32_t>(m_tracks.size()); }
  Position GetTrackStartMSFPosition(uint8_t track) const;
  LBA GetTrackLength(uint8_t track) const;
  TrackMode GetTrackMode(uint8_t track) const;
  uint32_t GetFirstTrackNumber() const { return m_tracks.front().track_number; }
  uint32_t GetLastTrackNumber() const { return m_tracks.back().track_number; }
  uint32_t GetIndexCount() const { return static_cast<uint32_t>(m_indices.size()); }
  const std::vector<Track>& GetTracks() const { return m_tracks; }
  const std::vector<Index>& GetIndices() const { return m_indices; }
  const Track& GetTrack(uint32_t track) const;
  const Index& GetIndex(uint32_t i) const;
  OpenFlags GetOpenFlags() const { return m_open_flags; }

  // Seek to data LBA.
  bool Seek(LBA lba);

  // Seek to track and position.
  bool Seek(uint32_t track_number, const Position& pos_in_track);

  // Seek to track and LBA.
  bool Seek(uint32_t track_number, LBA lba);

  // Read from the current LBA. Returns the number of sectors read.
  uint32_t Read(ReadMode read_mode, uint32_t sector_count, void* buffer);

  // Read a single raw sector, and subchannel from the current LBA.
  bool ReadRawSector(void* buffer, SubChannelQ* subq);

  // Reads sub-channel Q for the specified index+LBA.
  virtual bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index);

  // Returns true if the image has replacement subchannel data.
  virtual bool HasNonStandardSubchannel() const;

  // False for an image that is a drive, or already in memory, of its own
  // (PSSwanStation's discs in USB drives): "load the image into memory" leaves
  // it as it is.
  virtual bool AllowsPreload() const { return true; }

  // Reads a single sector from an index.
  virtual bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) = 0;

  // Retrieve image metadata.
  virtual std::string GetMetadata(const std::string_view& type) const;

  // Returns true if this image type has sub-images (e.g. m3u).
  virtual bool HasSubImages() const;

  // Returns the number of sub-images in this image, if the format supports multiple.
  virtual uint32_t GetSubImageCount() const;

  // Returns the current sub-image index, if any.
  virtual uint32_t GetCurrentSubImage() const;

  // Changes the current sub-image. If this fails, the image state is unchanged.
  virtual bool SwitchSubImage(uint32_t index, Common::Error* error);

  // Retrieve sub-image metadata.
  virtual std::string GetSubImageMetadata(uint32_t index, const std::string_view& type) const;

protected:
  void ClearTOC();
  void CopyTOC(const CDImage* image);

  const Index* GetIndexForDiscPosition(LBA pos);
  const Index* GetIndexForTrackPosition(uint32_t track_number, LBA track_pos);

  /// Generates sub-channel Q given the specified position.
  bool GenerateSubChannelQ(SubChannelQ* subq, LBA lba);

  /// Generates sub-channel Q from the given index and index-offset.
  void GenerateSubChannelQ(SubChannelQ* subq, const Index& index, uint32_t index_offset);

  /// Synthesis of lead-out data.
  void AddLeadOutIndex();

  std::string m_filename;
  uint32_t m_lba_count = 0;

  std::vector<Track> m_tracks;
  std::vector<Index> m_indices;

private:
  // Position on disc.
  LBA m_position_on_disc = 0;

  // Position in index.
  const Index* m_current_index = nullptr;
  LBA m_position_in_index = 0;

  OpenFlags m_open_flags;
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(CDImage::OpenFlags);
