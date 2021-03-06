#pragma once

#include "generator/generate_info.hpp"
#include "generator/intermediate_elements.hpp"

#include "coding/buffered_file_writer.hpp"
#include "coding/file_reader.hpp"
#include "coding/file_writer.hpp"
#include "coding/mmap_reader.hpp"

#include "base/assert.hpp"
#include "base/control_flow.hpp"
#include "base/file_name_utils.hpp"
#include "base/logging.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/iostreams/device/mapped_file.hpp>

#include "defines.hpp"

// Classes for reading and writing any data in file with map of offsets for
// fast searching in memory by some key.
namespace generator
{
namespace cache
{
using Key = uint64_t;
static_assert(std::is_integral<Key>::value, "Key must be an integral type");

// Used to store all world nodes inside temporary index file.
// To find node by id, just calculate offset inside index file:
// offset_in_file = sizeof(LatLon) * node_ID
struct LatLon
{
  int32_t m_lat = 0;
  int32_t m_lon = 0;
};
static_assert(sizeof(LatLon) == 8, "Invalid structure size");
static_assert(std::is_trivially_copyable<LatLon>::value, "");

struct LatLonPos
{
  uint64_t m_pos = 0;
  int32_t m_lat = 0;
  int32_t m_lon = 0;
};
static_assert(sizeof(LatLonPos) == 16, "Invalid structure size");
static_assert(std::is_trivially_copyable<LatLonPos>::value, "");

class PointStorageWriterInterface
{
public:
  using Nodes = std::vector<std::pair<Key, NodeElement>>;

  virtual ~PointStorageWriterInterface() {}
  virtual void AddPoint(uint64_t id, double lat, double lon) = 0;
  virtual void AddPoints(Nodes const & nodes, bool concurrent) = 0;
  virtual uint64_t GetNumProcessedPoints() const = 0;
};

class PointStorageReaderInterface
{
public:
  virtual ~PointStorageReaderInterface() {}
  virtual bool GetPoint(uint64_t id, double & lat, double & lon) const = 0;
};

class IndexFileReader
{
public:
  using Value = uint64_t;

  IndexFileReader() = default;
  explicit IndexFileReader(std::string const & name);

  bool GetValueByKey(Key key, Value & value) const;

  template <typename ToDo>
  void ForEachByKey(Key k, ToDo && toDo) const
  {
    auto range = std::equal_range(m_elements.begin(), m_elements.end(), k, ElementComparator());
    for (; range.first != range.second; ++range.first)
    {
      if (toDo((*range.first).second) == base::ControlFlow::Break)
        break;
    }
  }

private:
  using Element = std::pair<Key, Value>;

  struct ElementComparator
  {
    bool operator()(Element const & r1, Element const & r2) const
    {
      return ((r1.first == r2.first) ? r1.second < r2.second : r1.first < r2.first);
    }
    bool operator()(Element const & r1, Key r2) const { return (r1.first < r2); }
    bool operator()(Key r1, Element const & r2) const { return (r1 < r2.first); }
  };

  std::vector<Element> m_elements;
};


class IndexFileWriter
{
public:
  using Value = uint64_t;

  explicit IndexFileWriter(std::string const & name);

  void WriteAll();
  void Add(Key k, Value const & v);

private:
  using Element = std::pair<Key, Value>;

  std::vector<Element> m_elements;
  FileWriter m_fileWriter;
};

class OSMElementCacheReader
{
public:
  explicit OSMElementCacheReader(std::string const & name);

  template <class Value>
  bool Read(Key id, Value & value) const
  {
    uint64_t pos = 0;
    if (!m_offsetsReader.GetValueByKey(id, pos))
    {
      LOG_SHORT(LWARNING, ("Can't find offset in file", m_name + OFFSET_EXT, "by id", id));
      return false;
    }

    uint32_t const valueSize = *(reinterpret_cast<uint32_t const *>(m_fileMap.data() + pos));
    size_t const valueOffset = pos + sizeof(uint32_t);
    MemReader reader(m_fileMap.data() + valueOffset, valueSize);
    value.Read(reader);
    return true;
  }

protected:
  boost::iostreams::mapped_file_source m_fileMap;
  IndexFileReader m_offsetsReader;
  std::string m_name;
};

class OSMElementCacheWriter
{
public:
  explicit OSMElementCacheWriter(std::string const & name);

  template <typename Value>
  void Write(Key id, Value const & value)
  {
    m_offsets.Add(id, m_currOffset);
    m_data.clear();
    MemWriter<decltype(m_data)> w(m_data);

    value.Write(w);

    ASSERT_LESS(m_data.size(), std::numeric_limits<uint32_t>::max(), ());
    uint32_t sz = static_cast<uint32_t>(m_data.size());
    m_fileWriter.Write(&sz, sizeof(sz));
    m_fileWriter.Write(m_data.data(), sz);
    m_currOffset += sizeof(sz) + sz;
  }

  template <typename Key, typename Value>
  void Write(std::vector<std::pair<Key, Value>> const & elements, bool /* concurrent */)
  {
    auto data = std::vector<uint8_t>{};
    data.reserve(elements.size() * 1024);

    auto elementsOffsets = std::vector<std::pair<Key, uint64_t>>{};
    elementsOffsets.reserve(elements.size());

    auto writer = MemWriter<decltype(data)>{data};
    for (auto const & element : elements)
    {
      auto const pos = writer.Pos();
      WriteValue(element.second, writer);
      elementsOffsets.emplace_back(element.first, pos);
    }

    uint64_t dataOffset = 0;

    {
      std::lock_guard<std::mutex> lock{m_fileWriterMutex};
      dataOffset = m_currOffset;
      m_fileWriter.Write(data.data(), data.size());
      m_currOffset += data.size();
    }

    {
      std::lock_guard<std::mutex> lock{m_offsetsMutex};
      for (auto const & elementOffset : elementsOffsets)
        m_offsets.Add(elementOffset.first, dataOffset + elementOffset.second);
    }
  }

  void SaveOffsets();

protected:
  BufferedFileWriter m_fileWriter;
  std::mutex m_fileWriterMutex;
  uint64_t m_currOffset{0};
  IndexFileWriter m_offsets;
  std::mutex m_offsetsMutex;
  std::string m_name;
  std::vector<uint8_t> m_data;

private:
  template <typename Value, typename Writer>
  void WriteValue(Value const & element, Writer & writer)
  {
    auto const sizePos = writer.Pos();
    auto elementDataSize = uint32_t{0};
    writer.Write(&elementDataSize, sizeof(elementDataSize));

    auto const elementDataPos = writer.Pos();
    element.Write(writer);

    auto const elementDataEndPos = writer.Pos();
    elementDataSize = base::checked_cast<uint32_t>(elementDataEndPos - elementDataPos);
    ASSERT_LESS(elementDataSize, std::numeric_limits<uint32_t>::max(), ());
    writer.Seek(sizePos);
    writer.Write(&elementDataSize, sizeof(elementDataSize));
    writer.Seek(elementDataEndPos);
  }
};

class IntermediateDataReader
{
public:
  IntermediateDataReader(feature::GenerateInfo const & info);

  // TODO |GetNode()|, |lat|, |lon| are used as y, x in real.
  bool GetNode(Key id, double & lat, double & lon) const { return m_nodes->GetPoint(id, lat, lon); }
  bool GetWay(Key id, WayElement & e) const { return m_ways.Read(id, e); }

  template <typename ToDo>
  void ForEachRelationByWay(Key id, ToDo && toDo) const
  {
    RelationProcessor<ToDo> processor(m_relations, std::forward<ToDo>(toDo));
    m_wayToRelations.ForEachByKey(id, processor);
  }

  template <typename ToDo>
  void ForEachRelationByWayCached(Key id, ToDo && toDo) const
  {
    CachedRelationProcessor<ToDo> processor(m_relations, std::forward<ToDo>(toDo));
    m_wayToRelations.ForEachByKey(id, processor);
  }

  template <typename ToDo>
  void ForEachRelationByNodeCached(Key id, ToDo && toDo) const
  {
    CachedRelationProcessor<ToDo> processor(m_relations, std::forward<ToDo>(toDo));
    m_nodeToRelations.ForEachByKey(id, processor);
  }

private:
  using CacheReader = cache::OSMElementCacheReader;

  template <typename Element, typename ToDo>
  class ElementProcessorBase
  {
  public:
    ElementProcessorBase(CacheReader const & reader, ToDo & toDo) : m_reader(reader), m_toDo(toDo)
    { }

    base::ControlFlow operator()(uint64_t id)
    {
      Element e;
      return m_reader.Read(id, e) ? m_toDo(id, e) : base::ControlFlow::Break;
    }

  protected:
    CacheReader const & m_reader;
    ToDo & m_toDo;
  };

  template <typename ToDo>
  struct RelationProcessor : public ElementProcessorBase<RelationElement, ToDo>
  {
    using Base = ElementProcessorBase<RelationElement, ToDo>;

    RelationProcessor(CacheReader const & reader, ToDo & toDo) : Base(reader, toDo) {}
  };

  template <typename ToDo>
  struct CachedRelationProcessor : public RelationProcessor<ToDo>
  {
    using Base = RelationProcessor<ToDo>;

    CachedRelationProcessor(CacheReader const & reader, ToDo & toDo) : Base(reader, toDo) {}
    base::ControlFlow operator()(uint64_t id) { return this->m_toDo(id, this->m_reader); }
  };

  std::unique_ptr<PointStorageReaderInterface> m_nodes;
  cache::OSMElementCacheReader m_ways;
  cache::OSMElementCacheReader m_relations;
  cache::IndexFileReader m_nodeToRelations;
  cache::IndexFileReader m_wayToRelations;
};

class IntermediateDataWriter
{
public:
  using Nodes = std::vector<std::pair<Key, NodeElement>>;
  using Ways = std::vector<std::pair<Key, WayElement>>;
  using Relations = std::vector<std::pair<Key, RelationElement>>;

  IntermediateDataWriter(PointStorageWriterInterface & nodes, feature::GenerateInfo const & info);

  void AddNode(Key id, double lat, double lon);
  void AddNodes(Nodes const & nodes, bool concurrent);
  void AddWay(Key id, WayElement const & e);
  void AddWays(Ways const & ways, bool concurrent);

  void AddRelation(Key id, RelationElement const & e);
  void AddRelations(Relations const & relations, bool concurrent);
  void SaveIndex();

  static void AddToIndex(cache::IndexFileWriter & index, Key relationId, std::vector<uint64_t> const & values)
  {
    for (auto const v : values)
      index.Add(v, relationId);
  }

private:
  template <typename Container>
  static void AddToIndex(cache::IndexFileWriter & index, Key relationId, Container const & values)
  {
    for (auto const & v : values)
      index.Add(v.first, relationId);
  }

  PointStorageWriterInterface & m_nodes;
  cache::OSMElementCacheWriter m_ways;
  cache::OSMElementCacheWriter m_relations;
  cache::IndexFileWriter m_nodeToRelations;
  std::mutex m_nodeToRelationsUpdateMutex;
  cache::IndexFileWriter m_wayToRelations;
  std::mutex m_wayToRelationsUpdateMutex;
};

std::unique_ptr<PointStorageReaderInterface>
CreatePointStorageReader(feature::GenerateInfo::NodeStorageType type, std::string const & name);

std::unique_ptr<PointStorageWriterInterface>
CreatePointStorageWriter(feature::GenerateInfo::NodeStorageType type, std::string const & name);

class IntermediateData
{
public:
  explicit IntermediateData(feature::GenerateInfo const & info);
  std::shared_ptr<IntermediateDataReader> const & GetCache() const;

private:
  feature::GenerateInfo const & m_info;
  std::shared_ptr<IntermediateDataReader> m_reader;

  DISALLOW_COPY(IntermediateData);
};
}  // namespace cache
}  // namespace generator
