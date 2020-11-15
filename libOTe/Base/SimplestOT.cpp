#include "SimplestOT.h"


#include <cryptoTools/Network/Channel.h>
#include <cryptoTools/Common/BitVector.h>
#include <cryptoTools/Crypto/RandomOracle.h>
#include "coproto/NativeProto.h"
#include "coproto/Macros.h"


#ifdef ENABLE_SIMPLESTOT
#ifdef ENABLE_RELIC
    #include <cryptoTools/Crypto/RCurve.h>
#else    
    #include <cryptoTools/Crypto/Curve.h>
#endif

namespace osuCrypto
{

#ifdef ENABLE_RELIC
    using Curve = REllipticCurve;
    using Point = REccPoint;
    using Brick = REccPoint;
    using Number = REccNumber;
#else    
    using Curve = EllipticCurve;
    using Point = EccPoint;
    using Brick = EccBrick;
    using Number = EccNumber;
#endif

    void SimplestOT::receive(
        const BitVector& choices,
        span<block> msg,
        PRNG& prng,
        Channel& chl)
    {
        Curve curve;
        Point g = curve.getGenerator();
        u64 pointSize = g.sizeBytes();
        u64 n = msg.size();

        block comm = oc::ZeroBlock, seed;
        Point A(curve);
        std::vector<u8> buff(pointSize + mUniformOTs * sizeof(block)), hashBuff(pointSize);
        chl.recv(buff.data(), buff.size());
        A.fromBytes(buff.data());

        if (mUniformOTs)
            memcpy(&comm, buff.data() + pointSize, sizeof(block));

        buff.resize(pointSize * n);
        auto buffIter = buff.data();

        std::vector<Number> b; b.reserve(n);;
        std::array<Point, 2> B{ curve, curve };
        for (u64 i = 0; i < n; ++i)
        {
            b.emplace_back(curve, prng);
            B[0] = g * b[i];
            B[1] = A + B[0];

            B[choices[i]].toBytes(buffIter); buffIter += pointSize;
        }

        chl.asyncSend(std::move(buff));
        if (mUniformOTs)
        {
            chl.recv(seed);
            if (neq(comm, mAesFixedKey.ecbEncBlock(seed) ^ seed))
                throw std::runtime_error("bad decommitment " LOCATION);
        }

        for (u64 i = 0; i < n; ++i)
        {
            B[0] = A * b[i];
            B[0].toBytes(hashBuff.data());
            RandomOracle ro(sizeof(block));
            ro.Update(hashBuff.data(), hashBuff.size());
            ro.Update(i);
            if (mUniformOTs) ro.Update(seed);
            ro.Final(msg[i]);
        }
    }

    coproto::Proto SimplestOT::receive(const BitVector& choices, span<block> messages, PRNG& prng)
    {
        struct RProto : public coproto::NativeProto
        {
            bool mUniformOTs;
            const BitVector& choices;
            span<block> msg;
            PRNG& prng;
            RProto(bool& u, const BitVector& c, span<block> m, PRNG& p)
                : mUniformOTs(u)
                , choices(c)
                , msg(m)
                , prng(p)
                , g(curve)
                , A(curve)
                , B({ curve, curve })
            {}
            Curve curve;
            Point g;
            u64 pointSize;
            u64 n;
            block comm = oc::ZeroBlock, seed;
            Point A;
            std::vector<u8> buff, hashBuff;
            std::vector<Number> b;
            std::array<Point, 2> B;
            u8* buffIter;
            coproto::error_code resume() override
            {
                Curve curve;
                CP_BEGIN();
                g = curve.getGenerator();
                pointSize = g.sizeBytes();
                n = msg.size();

                buff.resize(pointSize + mUniformOTs * sizeof(block));
                hashBuff.resize(pointSize);
                

                CP_RECV(buff);
                A.fromBytes(buff.data());

                if (mUniformOTs)
                    memcpy(&comm, buff.data() + pointSize, sizeof(block));

                buff.resize(pointSize * n);
                buffIter = buff.data();
                b.reserve(n);

                for (u64 i = 0; i < n; ++i)
                {
                    b.emplace_back(curve, prng);
                    B[0] = g * b[i];
                    B[1] = A + B[0];

                    B[choices[i]].toBytes(buffIter); buffIter += pointSize;
                }


                CP_SEND(std::move(buff));
                if (mUniformOTs)
                {
                    CP_RECV(seed);

                    //chl.recv(seed);
                    if (neq(comm, mAesFixedKey.ecbEncBlock(seed) ^ seed))
                        throw std::runtime_error("bad decommitment " LOCATION);
                }

                for (u64 i = 0; i < n; ++i)
                {
                    B[0] = A * b[i];
                    B[0].toBytes(hashBuff.data());
                    RandomOracle ro(sizeof(block));
                    ro.Update(hashBuff.data(), hashBuff.size());
                    ro.Update(i);
                    if (mUniformOTs) ro.Update(seed);
                    ro.Final(msg[i]);
                }


                CP_END();
                return {};
            }
        };

        return coproto::makeProto<RProto>(mUniformOTs, choices, messages, prng);
    }



    void SimplestOT::send(
        span<std::array<block, 2>> msg,
        PRNG& prng,
        Channel& chl)
    {
        Curve curve;
        Point g = curve.getGenerator();
        u64 pointSize = g.sizeBytes();
        u64 n = msg.size();

        block seed = prng.get<block>();
        Number a(curve, prng);
        Point A = g * a;
        std::vector<u8> buff(pointSize + mUniformOTs * sizeof(block)), hashBuff(pointSize);
        A.toBytes(buff.data());

        if (mUniformOTs)
        {
            // commit to the seed
            auto comm = mAesFixedKey.ecbEncBlock(seed) ^ seed;
            memcpy(buff.data() + pointSize, &comm, sizeof(block));
        }

        chl.asyncSend(std::move(buff));

        buff.resize(pointSize * n);
        chl.recv(buff.data(), buff.size());

        if (mUniformOTs)
        {
            // decommit to the seed now that we have their messages.
            chl.send(seed);
        }

        auto buffIter = buff.data();

        A *= a;
        Point B(curve), Ba(curve);
        for (u64 i = 0; i < n; ++i)
        {
            B.fromBytes(buffIter); buffIter += pointSize;

            Ba = B * a;
            Ba.toBytes(hashBuff.data());
            RandomOracle ro(sizeof(block));
            ro.Update(hashBuff.data(), hashBuff.size());
            ro.Update(i);
            if (mUniformOTs) ro.Update(seed);
            ro.Final(msg[i][0]);

            Ba -= A;
            Ba.toBytes(hashBuff.data());
            ro.Reset();
            ro.Update(hashBuff.data(), hashBuff.size());
            ro.Update(i);
            if (mUniformOTs) ro.Update(seed);
            ro.Final(msg[i][1]);
        }
    }


    coproto::Proto SimplestOT::send(span<std::array<block, 2>> messages, PRNG& prng)
    {
        struct SProto : public coproto::NativeProto
        {
            bool mUniform;
            span<std::array<block, 2>> msg;
            PRNG& prng;
            SProto(bool uniform, span<std::array<block, 2>> m, PRNG& p)
                :mUniform(uniform)
                , msg(m)
                , prng(p)
                , g(curve)
                , a(curve)
                , A(curve)
                , B(curve)
                , Ba(curve)
            {}

            Curve curve;
            Point g;
            u64 pointSize;
            u64 n;
            block seed;
            Number a;
            Point A;
            std::vector<u8> buff, hashBuff;
            u8* buffIter;
            Point B, Ba;
            coproto::error_code resume() override
            {
                Curve curve;
                CP_BEGIN();
                g = curve.getGenerator();
                pointSize = g.sizeBytes();
                n = msg.size();

                seed = prng.get<block>();
                a.randomize(prng);
                A = g * a;
                buff.resize(pointSize + mUniform * sizeof(block));
                hashBuff.resize(pointSize);
                A.toBytes(buff.data());

                if (mUniform)
                {
                    // commit to the seed
                    auto comm = mAesFixedKey.ecbEncBlock(seed) ^ seed;
                    memcpy(buff.data() + pointSize, &comm, sizeof(block));
                }

                CP_SEND(std::move(buff));

                buff.resize(pointSize * n);
                CP_RECV(buff);

                if (mUniform)
                {
                    // decommit to the seed now that we have their messages.
                    CP_SEND(seed);
                }

                buffIter = buff.data();

                A *= a;
                for (u64 i = 0; i < n; ++i)
                {
                    B.fromBytes(buffIter); buffIter += pointSize;

                    Ba = B * a;
                    Ba.toBytes(hashBuff.data());
                    RandomOracle ro(sizeof(block));
                    ro.Update(hashBuff.data(), hashBuff.size());
                    ro.Update(i);
                    if (mUniform) ro.Update(seed);
                    ro.Final(msg[i][0]);

                    Ba -= A;
                    Ba.toBytes(hashBuff.data());
                    ro.Reset();
                    ro.Update(hashBuff.data(), hashBuff.size());
                    ro.Update(i);
                    if (mUniform) ro.Update(seed);
                    ro.Final(msg[i][1]);
                }

                CP_END();
                return { };
            }

        };
        return coproto::makeProto<SProto>(mUniformOTs, messages, prng);
    }


}
#endif

#ifdef ENABLE_SIMPLESTOT_ASM
extern "C"
{
    #include "../SimplestOT/ot_sender.h"
    #include "../SimplestOT/ot_receiver.h"
    #include "../SimplestOT/ot_config.h"
    #include "../SimplestOT/cpucycles.h"
    #include "../SimplestOT/randombytes.h"
}
namespace osuCrypto
{

    rand_source makeRandSource(PRNG& prng)
    {
        rand_source rand;
        rand.get = [](void* ctx, unsigned char* dest, unsigned long long length) {
            PRNG& prng = *(PRNG*)ctx;
            prng.get(dest, length);
        };
        rand.ctx = &prng;

        return rand;
    }

    void AsmSimplestOT::receive(
        const BitVector& choices,
        span<block> msg,
        PRNG& prng,
        Channel& chl)
    {
        RECEIVER receiver;

        u8 Rs_pack[4 * SIMPLEST_OT_PACK_BYTES];
        u8 keys[4][SIMPLEST_OT_HASHBYTES];
        u8 cs[4];

        chl.recv(receiver.S_pack, sizeof(receiver.S_pack));
        receiver_procS(&receiver);

        receiver_maketable(&receiver);
        auto rand = makeRandSource(prng);

        for (u32 i = 0; i < msg.size(); i += 4)
        {
            auto min = std::min<u32>(4, msg.size() - i);

            for (u32 j = 0; j < min; j++)
                cs[j] = choices[i + j];
            
            receiver_rsgen(&receiver, Rs_pack, cs, rand);
            chl.asyncSendCopy(Rs_pack, sizeof(Rs_pack));
            receiver_keygen(&receiver, keys);

            for (u32 j = 0; j < min; j++)
                memcpy(&msg[i + j], keys[j], sizeof(block));
        }
    }

    void AsmSimplestOT::send(
        span<std::array<block, 2>> msg,
        PRNG& prng,
        Channel& chl)
    {
        SENDER sender;

        u8 S_pack[SIMPLEST_OT_PACK_BYTES];
        u8 Rs_pack[4 * SIMPLEST_OT_PACK_BYTES];
        u8 keys[2][4][SIMPLEST_OT_HASHBYTES];

        auto rand = makeRandSource(prng);
        sender_genS(&sender, S_pack, rand);
        chl.asyncSend(S_pack, sizeof(S_pack));

        for (u32 i = 0; i < msg.size(); i += 4)
        {
            chl.recv(Rs_pack, sizeof(Rs_pack));
            sender_keygen(&sender, Rs_pack, keys);

            auto min = std::min<u32>(4, msg.size() - i);
            for (u32 j = 0; j < min; j++)
            {
                memcpy(&msg[i + j][0], keys[0][j], sizeof(block));
                memcpy(&msg[i + j][1], keys[1][j], sizeof(block));
            }
        }
    }
}
#endif




