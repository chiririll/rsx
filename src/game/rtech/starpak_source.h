#pragma once

// Abstract byte-range reader for starpak (and other depot) payloads.
// Local disk and Steam CDN backends share this interface so CPakFile can
// stream asset data without knowing where the bytes came from.
class CStarPakSource
{
public:
	virtual ~CStarPakSource() = default;

	virtual uint64_t size() const = 0;

	// Must be const-callable: getStarPakData() is const and only ever sees a
	// const StarPak_t*. Remote backends keep mutable state (cache, in-flight
	// downloads) behind a mutable mutex.
	virtual std::unique_ptr<char[]> readAt(uint64_t offset, uint64_t size) const = 0;
};

class CLocalStarPakSource : public CStarPakSource
{
public:
	explicit CLocalStarPakSource(std::filesystem::path path);

	uint64_t size() const override;
	std::unique_ptr<char[]> readAt(uint64_t offset, uint64_t size) const override;

	const std::filesystem::path& path() const { return m_path; }

private:
	std::filesystem::path m_path;
	uint64_t m_size = 0;
};

// Placeholder used when a starpak cannot be resolved, so positional starpak
// indices in the owning CPakFile stay aligned with asset references.
class CNullStarPakSource : public CStarPakSource
{
public:
	uint64_t size() const override { return 0; }
	std::unique_ptr<char[]> readAt(uint64_t /*offset*/, uint64_t /*size*/) const override { return nullptr; }
};
