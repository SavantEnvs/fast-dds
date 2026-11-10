// Feeds one RTPS datagram to a MessageReceiver that has real best-effort (StatelessReader) readers
// registered, so DATA_FRAG submessages get past the "no reader for this entity id" exit that stops
// upstream's fuzz_processCDRMsg (it registers no reader) and reach proc_Submsg_DataFrag and
// StatelessReader::process_data_frag_msg: the receive path of a best-effort DataReader fed by UDP.
//
// The readers are built like a DataReader builds its RTPS reader (topic payload pool from the history
// attributes, data sharing off, user-defined entity ids) and a writer is matched with
// matched_writer_add, as discovery would. One reader per memory policy, since the policy decides how
// a reassembly buffer is sized. A fresh set per input keeps each result independent of earlier inputs.
//
// Fixed identities a datagram must carry to reach a reader (the seeds have them):
//   writer GUID  prefix 01 02 03 04 05 06 07 08 09 0a 0b 0c, entity id 00 00 01 03
//   reader ids   00 00 10 04 PREALLOCATED, 00 00 11 04 PREALLOCATED_WITH_REALLOC,
//                00 00 12 04 DYNAMIC_RESERVE, 00 00 13 04 DYNAMIC_REUSABLE
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/RTPSDomain.hpp>
#include <fastdds/rtps/attributes/HistoryAttributes.hpp>
#include <fastdds/rtps/attributes/ReaderAttributes.hpp>
#include <fastdds/rtps/attributes/RTPSParticipantAttributes.hpp>
#include <fastdds/rtps/builtin/data/PublicationBuiltinTopicData.hpp>
#include <fastdds/rtps/common/CDRMessage_t.hpp>
#include <fastdds/rtps/common/Guid.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/history/ReaderHistory.hpp>
#include <fastdds/rtps/participant/RTPSParticipant.hpp>
#include <fastdds/rtps/reader/RTPSReader.hpp>

#include <rtps/history/PoolConfig.h>
#include <rtps/history/TopicPayloadPoolRegistry.hpp>

#include <MessageReceiver.h>

using namespace eprosima::fastdds;
using namespace eprosima::fastdds::rtps;

#define MIN_SIZE RTPSMESSAGE_HEADER_SIZE
#define MAX_SIZE 64000

// A DATA_FRAG that declares a huge sample size makes a dynamic-memory reader allocate that much, which
// only ever reports out-of-memory. Such datagrams are skipped (not counted in the corpus).
static constexpr uint32_t kMaxSampleSize = 1u << 20;

static bool declares_huge_sample(
        const uint8_t* data,
        size_t size)
{
    size_t pos = RTPSMESSAGE_HEADER_SIZE;
    while (pos + 4 <= size)
    {
        const uint8_t id = data[pos];
        const bool little = (data[pos + 1] & 1) != 0;
        size_t len = little ? (data[pos + 2] | (data[pos + 3] << 8)) : ((data[pos + 2] << 8) | data[pos + 3]);
        if (id == 0x16 && pos + 4 + 32 <= size)
        {
            const uint8_t* p = data + pos + 4 + 28;
            const uint32_t sample_size = little
                ? (p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24))
                : ((static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
            if (sample_size > kMaxSampleSize)
            {
                return true;
            }
        }
        if (0 == len)
        {
            break;
        }
        pos += 4 + len;
    }
    return false;
}

static RTPSParticipant* participant = nullptr;

static const MemoryManagementPolicy_t policies[] = {
    PREALLOCATED_MEMORY_MODE,
    PREALLOCATED_WITH_REALLOC_MEMORY_MODE,
    DYNAMIC_RESERVE_MEMORY_MODE,
    DYNAMIC_REUSABLE_MEMORY_MODE
};
static constexpr size_t kReaders = sizeof(policies) / sizeof(policies[0]);

static GUID_t writer_guid()
{
    GUID_t guid;
    for (int i = 0; i < 12; ++i)
    {
        guid.guidPrefix.value[i] = static_cast<octet>(i + 1);
    }
    guid.entityId = EntityId_t(0x00000103u);
    return guid;
}

struct FuzzReader
{
    HistoryAttributes hatt;
    std::unique_ptr<ReaderHistory> history;
    std::shared_ptr<ITopicPayloadPool> pool;
    PoolConfig config;
    RTPSReader* reader = nullptr;

    bool create(
            size_t index)
    {
        hatt.memoryPolicy = policies[index];
        hatt.payloadMaxSize = 500;
        hatt.initialReservedCaches = 2;
        hatt.maximumReservedCaches = 0;
        history.reset(new ReaderHistory(hatt));

        config = PoolConfig::from_history_attributes(hatt);
        pool = TopicPayloadPoolRegistry::get("fuzz_topic", config);
        pool->reserve_history(config, true);

        ReaderAttributes att;
        att.endpoint.endpointKind = READER;
        att.endpoint.reliabilityKind = BEST_EFFORT;
        att.endpoint.durabilityKind = VOLATILE;
        att.endpoint.topicKind = NO_KEY;
        dds::DataSharingQosPolicy datasharing;
        datasharing.off();
        att.endpoint.set_data_sharing_configuration(datasharing);

        EntityId_t id(static_cast<uint32_t>(0x00001004u + (index << 8)));
        reader = RTPSDomain::createRTPSReader(participant, id, att, pool, history.get(), nullptr);
        if (nullptr == reader)
        {
            pool->release_history(config, true);
            return false;
        }

        PublicationBuiltinTopicData writer;
        writer.guid = writer_guid();
        writer.participant_guid = writer.guid;
        writer.participant_guid.entityId = c_EntityId_RTPSParticipant;
        writer.topic_name = "fuzz_topic";
        writer.type_name = "fuzz_type";
        writer.reliability.kind = dds::BEST_EFFORT_RELIABILITY_QOS;
        writer.durability.kind = dds::VOLATILE_DURABILITY_QOS;
        reader->matched_writer_add(writer);
        return true;
    }

    ~FuzzReader()
    {
        if (reader)
        {
            RTPSDomain::removeRTPSReader(reader);
            pool->release_history(config, true);
        }
    }
};

extern "C" int LLVMFuzzerInitialize(
        int* /*argc*/,
        char*** /*argv*/)
{
    eprosima::fastdds::dds::Log::ClearConsumers();

    // Not enabled: no transports, sockets or discovery; only the RTPS objects a reader needs.
    RTPSParticipantAttributes attrs;
    participant = RTPSDomain::createParticipant(0, false, attrs, nullptr);
    if (nullptr == participant)
    {
        abort();
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(
        const uint8_t* data,
        size_t size)
{
    if (size < MIN_SIZE || size > MAX_SIZE)
    {
        return -1;
    }
    if (declares_huge_sample(data, size))
    {
        return -1;
    }

    FuzzReader readers[kReaders];
    MessageReceiver rcv(nullptr, MAX_SIZE);
    for (size_t i = 0; i < kReaders; ++i)
    {
        if (!readers[i].create(i))
        {
            abort();
        }
        rcv.associateEndpoint(readers[i].reader);
    }

    CDRMessage_t msg(0);
    msg.wraps = true;
    msg.buffer = const_cast<octet*>(data);
    msg.length = static_cast<uint32_t>(size);
    msg.max_size = static_cast<uint32_t>(size);
    msg.reserved_size = static_cast<uint32_t>(size);

    Locator_t locator;
    rcv.processCDRMsg(locator, locator, &msg);

    for (size_t i = 0; i < kReaders; ++i)
    {
        rcv.removeEndpoint(readers[i].reader);
    }
    return 0;
}
