// Cordinal's SILK reader and floating point prediction/synthesis, RFC 6716 section 4.2.
// Implements OpusSilk: NLSF/LPC and LTP decoding, pitch (voiced) prediction,
// stereo unmixing, packet-loss concealment, and the resampler that lifts the
// internal 8/12/16 kHz signal to 48 kHz. Fixed-point parsing feeds float synthesis.
#include "audio_opus_silk.h"
#include "audio_opus_silk_tables.h"
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#ifdef CORDINAL_CODEC_TRACE
#include <cstdio>
#endif

namespace nativeaudio {
namespace {
using namespace silkdata;
// An entropy model is a cumulative iCDF: counts in [0,255] ending in 0.
using Model=std::span<const uint8_t>;
constexpr double pi=std::numbers::pi;
int Sign(int v) { return (v>0)-(v<0); }
double Clip(double v) { return std::clamp(v,-1.0,1.0); }
// Rounding right shift (half up), matching the reference fixed-point arithmetic.
int Rounded(int64_t v,unsigned shift) { return (int)((v+(int64_t(1)<<(shift-1)))>>shift); }

// NLSF codebook bundle: the narrow (NB/MB) and wide (WB) sets share this layout.
struct LsfBook {
    unsigned order;
    const uint8_t *vectors,*first,*select,*residual,*predict;
    const int16_t *spacing;
};
const LsfBook narrow{10,NLSF_CB1_NB_MB_Q8,NLSF_CB1_iCDF_NB_MB,NLSF_CB2_SELECT_NB_MB,
    NLSF_CB2_iCDF_NB_MB,NLSF_PRED_NB_MB_Q8,NLSF_DELTA_MIN_NB_MB_Q15};
const LsfBook wide{16,NLSF_CB1_WB_Q8,NLSF_CB1_iCDF_WB,NLSF_CB2_SELECT_WB,
    NLSF_CB2_iCDF_WB,NLSF_PRED_WB_Q8,NLSF_DELTA_MIN_WB_Q15};

// Enforces the RFC's minimum spacing between adjacent NLSFs, repairing any band
// that violates it (RFC 4.2.7.5.6).
void Stabilize(std::array<int,16>& lsf,const LsfBook& book) {
    int n=book.order;
    for (int pass=0;pass<20;++pass) {
        int worst=0,gap=lsf[0]-book.spacing[0];
        for (int i=1;i<=n;++i) {
            int next=(i==n?32768:lsf[i])-lsf[i-1]-book.spacing[i];
            if (next<gap) { gap=next; worst=i; }
        }
        if (gap>=0) return;
        if (!worst) lsf[0]=book.spacing[0];
        else if (worst==n) lsf[n-1]=32768-book.spacing[n];
        else {
            int lo=book.spacing[worst]/2,hi=32768-lo;
            for (int k=0;k<worst;++k) lo+=book.spacing[k];
            for (int k=worst+1;k<=n;++k) hi-=book.spacing[k];
            int center=std::clamp((lsf[worst-1]+lsf[worst]+1)>>1,lo,hi);
            lsf[worst-1]=center-book.spacing[worst]/2;
            lsf[worst]=lsf[worst-1]+book.spacing[worst];
        }
    }
    std::sort(lsf.begin(),lsf.begin()+n);
    lsf[0]=std::max(lsf[0],(int)book.spacing[0]);
    for (int i=1;i<n;++i) lsf[i]=std::max(lsf[i],lsf[i-1]+book.spacing[i]);
    lsf[n-1]=std::min(lsf[n-1],32768-book.spacing[n]);
    for (int i=n-2;i>=0;--i) lsf[i]=std::min(lsf[i],lsf[i+1]-book.spacing[i+1]);
}

// Decodes one NLSF vector: stage-1 codebook index, per-coefficient residual via
// the stage-2 prediction, then reconstruction to Q15 and stabilization.
std::array<int,16> ReadLsf(OpusRange& r,const LsfBook& book,unsigned type) {
    unsigned d=book.order,index=r.Icdf({book.first+(type==2?32:0),32});
    std::array<int,16> indices{},prediction{},lsf{};
    for (unsigned i=0;i<d;++i) {
        unsigned choice=(book.select[index*d/2+i/2]>>((i&1)*4))&15;
        indices[i]=(int)r.Icdf({book.residual+9*(choice>>1),9})-4;
        if (std::abs(indices[i])==4) indices[i]+=Sign(indices[i])*(int)r.Icdf(NLSF_EXT_iCDF);
        prediction[i]=i+1<d?book.predict[i+(choice&1)*(d-1)]:0;
    }
    const uint8_t* cb=book.vectors+index*d;
    int residual=0;
    for (int i=(int)d-1;i>=0;--i) {
        residual=((residual*prediction[i])>>8)+(((indices[i]*1024-Sign(indices[i])*102)*(d==16?9830:11796))>>16);
        int left=cb[i]-(i?cb[i-1]:0),right=(i+1<(int)d?cb[i+1]:256)-cb[i];
        uint32_t squared=(1024/left+1024/right)*65536u;
        unsigned log=32-std::countl_zero(squared),f=(squared>>(log-8))&127;
        int y=((log&1)?32768:46214)>>((32-log)>>1);
        int weight=y+((213*(int)f*y)>>16);
        lsf[i]=std::clamp((int)cb[i]*128+residual*16384/weight,0,32767);
    }
    Stabilize(lsf,book); return lsf;
}

// Chirp/bandwidth expansion: scales a[i] by chirp^(i+1) in Q16.
void Expand(std::array<int32_t,16>& a,unsigned order,int chirp) {
    int64_t scale=chirp;
    for (unsigned i=0;i<order;++i) {
        a[i]=(int32_t)((a[i]*scale)>>16);
        scale=(scale*chirp+32768)>>16;
    }
}

// Tests LPC filter stability via the Levinson recursion (RFC 4.2.7.5.6);
// returns false if any reflection coefficient reaches magnitude 1.
bool Stable(const std::array<int32_t,16>& coefficients,unsigned order) {
    std::array<int64_t,16> a{},next{};
    int64_t dc=0,inverse=1ll<<30;
    for (unsigned i=0;i<order;++i) { int q=Rounded(coefficients[i],5); dc+=q; a[i]=q*4096ll; }
    if (dc>=4096) return false;
    for (int k=(int)order-1;k>=0;--k) {
        if (std::abs(a[k])>16773022) return false;
        int64_t reflection=-a[k]*128,denom=(1ll<<30)-((reflection*reflection)>>32);
        inverse=((inverse*denom)>>32)*4;
        if (inverse<107374) return false;
        if (!k) break;
        unsigned log=64-std::countl_zero((uint64_t)denom),bits=log-16;
        int64_t estimate=((1ll<<29)-1)/(denom>>(bits+1));
        int64_t error=(1ll<<29)-(((denom<<(15-bits))*estimate)>>16);
        int64_t gain=estimate*65536+((error*estimate)>>13);
        for (int i=0;i<k;++i) {
            int64_t numerator=a[i]-((a[k-i-1]*reflection+(1ll<<30))>>31);
            next[i]=std::clamp<int64_t>((numerator*gain+(1ll<<(log-1)))>>log,INT32_MIN,INT32_MAX);
        }
        a=next;
    }
    return true;
}

// Converts NLSFs to LPC coefficients (RFC 4.2.7.5.6): builds the two polynomials
// from the roots, then bandwidth-expands until the filter is stable.
std::array<double,16> Lpc(const std::array<int,16>& lsf,unsigned d) {
    // These are the normative piecewise cosine values, which deliberately
    // differ from a freshly rounded cosine at several indices.
    const auto& cosines=LSFCosTab_FIX_Q12;
    constexpr unsigned order10[]{0,9,6,3,4,5,8,1,2,7};
    constexpr unsigned order16[]{0,15,8,7,4,11,12,3,2,13,10,5,6,9,14,1};
    std::array<int,16> roots{};
    for (unsigned i=0;i<d;++i) {
        unsigned index=lsf[i]>>8,fraction=lsf[i]&255;
        roots[(d==16?order16:order10)[i]]=(cosines[index]*256+(cosines[index+1]-cosines[index])*(int)fraction+8)>>4;
    }
    auto polynomial=[&](unsigned parity) {
        std::array<int64_t,9> p{}; p[0]=65536;p[1]=-roots[parity];
        for (unsigned k=1;k<d/2;++k) {
            auto old=p; old[k+1]=old[k-1];
            for (unsigned j=1;j<=k+1;++j)
                p[j]=old[j]+(j>=2?old[j-2]:0)-((int64_t(roots[2*k+parity])*old[j-1]+32768)>>16);
        }
        return p;
    };
    auto p=polynomial(0),q=polynomial(1);
    std::array<int32_t,16> a{};
    for (unsigned k=0;k<d/2;++k) {
        int64_t sum=p[k+1]+p[k],difference=q[k+1]-q[k];
        a[k]=(int32_t)(-sum-difference);a[d-1-k]=(int32_t)(difference-sum);
    }
    for (unsigned pass=0;pass<10;++pass) {
        unsigned index=0;
        for (unsigned i=1;i<d;++i) if (std::abs(int64_t(a[i]))>std::abs(int64_t(a[index]))) index=i;
        int largest=(int)std::min<int64_t>((std::abs(int64_t(a[index]))+16)>>5,163838);
        if (largest<=32767) break;
        Expand(a,d,65470-(int)(((int64_t(largest)-32767)*16384)/((int64_t(largest)*(index+1))>>2)));
        if (pass==9) for (unsigned i=0;i<d;++i) a[i]=std::clamp(Rounded(a[i],5),-32768,32767)*32;
    }
    for (unsigned pass=0;pass<16 && !Stable(a,d);++pass) Expand(a,d,65536-(2<<pass));
    std::array<double,16> out{};
    for (unsigned i=0;i<d;++i) out[i]=Rounded(a[i],5)/4096.0;
#ifdef CORDINAL_CODEC_TRACE
    if (std::getenv("CORDINAL_SILK_NATIVE")) {
        fprintf(stderr,"lpc");for (unsigned i=0;i<d;++i) fprintf(stderr," %d",Rounded(a[i],5));fprintf(stderr,"\n");
    }
#endif
    return out;
}

// Maps a decoded log-gain index (0-63) to a linear Q16 gain via the RFC's
// piecewise exponential (a fractional-power approximation).
double Gain(int log) {
    int value=(0x1d1c71*log>>16)+2090;
    int64_t base=1ll<<(value>>7),fraction=value&127;
    return (base+(((-174*fraction*(128-fraction)>>16)+fraction)*(base>>7)))/65536.0;
}

// All decoded parameters for one SILK frame: gains, NLSF/interpolation, pitch and
// LTP (voiced frames), excitation pulses, plus the type/offset and LTP scale.
struct Parameters {
    unsigned type=0,offset=0,interpolation=4,seed=0,order=0;
    std::array<double,4> gains{};
    std::array<int,4> pitch{};
    std::array<std::array<double,5>,4> ltp{};
    std::array<int,16> lsf{};
    std::array<int,320> pulses{};
    double scale=15565.0/16384;
};
// Per-channel persistent synthesis state. pcm/scaled hold 512 samples of history
// (10 ms at 48 kHz) followed by up to one 320-sample frame, for LTP lookback.
struct Channel {
    int logGain=0,lag=0,type=0;
    bool have=false;
    std::array<int,16> lsf{};
    std::array<double,832> pcm{};
    std::array<int32_t,832> scaled{};
    int64_t previousGain=65536;
};

// Decodes a "shell" combinatorial code (RFC 4.2.7.8.1): recursively splits
// `count` pulses over `size` positions using the size-specific code table.
void Shell(OpusRange& r,int* pulses,unsigned count,unsigned size) {
    if (!count) { std::fill_n(pulses,size,0); return; }
    if (size==1) { *pulses=count;return; }
    const uint8_t* table=size==16?shell_code_table3:size==8?shell_code_table2:size==4?shell_code_table1:shell_code_table0;
    unsigned left=r.Icdf({table+shell_code_table_offsets[count],count+1});
    Shell(r,pulses,left,size/2); Shell(r,pulses+size/2,count-left,size/2);
}

// Reads one frame's excitation: per-16-block pulse counts (with escalation when a
// block reports 17), the shell-coded positions, LSB refinement and pulse signs.
void ReadPulses(OpusRange& r,Parameters& p,unsigned samples) {
    unsigned level=r.Icdf({rate_levels_iCDF+(p.type==2?9:0),9}),blocks=(samples+15)/16;
    std::array<unsigned,20> sums{},extra{};
    for (unsigned b=0;b<blocks;++b) {
        sums[b]=r.Icdf({pulses_per_block_iCDF+18*level,18});
        while (sums[b]==17) {
            if (++extra[b]>10) { r.Fail();return; }
            unsigned shift=extra[b]==10?1:0;
            sums[b]=r.Icdf({pulses_per_block_iCDF+9*18+shift,18-shift});
        }
    }
    for (unsigned b=0;b<blocks;++b) Shell(r,p.pulses.data()+b*16,sums[b],16);
    for (unsigned b=0;b<blocks;++b) for (unsigned j=0;j<16;++j)
        for (unsigned bit=0;bit<extra[b];++bit) p.pulses[b*16+j]=p.pulses[b*16+j]*2+r.Icdf(lsb_iCDF);
    for (unsigned b=0;b<blocks;++b) {
        uint8_t model[]{sign_iCDF[7*(2*p.type+p.offset)+std::min(sums[b],6u)],0};
        for (unsigned j=0;j<16;++j) if (p.pulses[b*16+j] && !r.Icdf(model)) p.pulses[b*16+j]=-p.pulses[b*16+j];
    }
}

// Reads one SILK frame's parameters. `active` uses the VAD-gated type code space;
// `conditional` enables inter-frame delta coding (gains, pitch, LTP).
Parameters ReadParameters(OpusRange& r,Channel& state,unsigned rate,unsigned subframes,bool active,bool conditional,bool scale) {
    Parameters p;
    unsigned type=active?r.Icdf(type_offset_VAD_iCDF)+2:r.Icdf(type_offset_no_VAD_iCDF);
    p.type=type/2;p.offset=type&1;
    for (unsigned s=0;s<subframes;++s) {
        if (!s && !conditional) {
            unsigned high=r.Icdf({gain_iCDF+p.type*8,8});
            int gain=high*8+r.Icdf(uniform8_iCDF);
            state.logGain=state.have?std::max(gain,state.logGain-16):gain;
        } else {
            int delta=r.Icdf(delta_gain_iCDF);
            state.logGain=std::clamp(std::max(2*delta-16,state.logGain+delta-4),0,63);
        }
        p.gains[s]=Gain(state.logGain);
    }
    const auto& book=rate==16000?wide:narrow; p.order=book.order;
    p.lsf=ReadLsf(r,book,p.type);
    if (subframes==4) p.interpolation=r.Icdf(NLSF_interpolation_factor_iCDF);
    if (!state.have) p.interpolation=4;
    if (p.type==2) {
        int delta=conditional && state.type==2?(int)r.Icdf(pitch_delta_iCDF):0;
        unsigned khz=rate/1000;
        if (delta) state.lag+=delta-9;
        else {
            unsigned coarse=r.Icdf(pitch_lag_iCDF);
            unsigned fine=khz==8?r.Icdf(uniform4_iCDF):khz==12?r.Icdf(uniform6_iCDF):r.Icdf(uniform8_iCDF);
            state.lag=coarse*(khz/2)+fine+2*khz;
        }
        Model model=khz==8?(subframes==2?Model(pitch_contour_10_ms_NB_iCDF):Model(pitch_contour_NB_iCDF)):
            (subframes==2?Model(pitch_contour_10_ms_iCDF):Model(pitch_contour_iCDF));
        const int8_t* contour=khz==8?(subframes==2?CB_lags_stage2_10_ms:CB_lags_stage2):
            (subframes==2?CB_lags_stage3_10_ms:CB_lags_stage3);
        unsigned choice=r.Icdf(model);
        for (unsigned s=0;s<subframes;++s) p.pitch[s]=std::clamp(state.lag+contour[s*model.size()+choice],int(2*khz),int(18*khz));
        unsigned family=r.Icdf(LTP_per_index_iCDF);
        Model ltpModel=family==0?Model(LTP_gain_iCDF_0):family==1?Model(LTP_gain_iCDF_1):Model(LTP_gain_iCDF_2);
        const int8_t* cb=family==0?LTP_gain_vq_0:family==1?LTP_gain_vq_1:LTP_gain_vq_2;
        for (unsigned s=0;s<subframes;++s) {
            unsigned index=r.Icdf(ltpModel);
            for (unsigned k=0;k<5;++k) p.ltp[s][k]=cb[index*5+k]/128.0;
        }
        if (scale) p.scale=LTPScales_table_Q14[r.Icdf(LTPscale_iCDF)]/16384.0;
    }
    state.type=p.type;
    p.seed=r.Icdf(uniform4_iCDF);
    ReadPulses(r,p,subframes*rate/200);
    return p;
}

// Decodes the Q13 stereo prediction weights used to rebuild left/right from mid/side.
std::array<int,2> StereoWeights(OpusRange& r) {
    unsigned joint=r.Icdf(stereo_pred_joint_iCDF);
    std::array<int,2> w{};
    for (unsigned ch=0;ch<2;++ch) {
        unsigned index=r.Icdf(uniform3_iCDF)+3*(ch?joint%5:joint/5),fraction=r.Icdf(uniform5_iCDF);
        w[ch]=stereo_pred_quant_Q13[index]+(((stereo_pred_quant_Q13[index+1]-stereo_pred_quant_Q13[index])*6554)>>16)*(2*(int)fraction+1);
    }
    w[0]-=w[1];return w;
}

// Runs LPC/LTP excitation and synthesis for one channel at `rate` for subframes
// (rate/200 samples each). Returns the new samples and shifts the 512-sample
// history window forward; output stays at `rate` and Resample() lifts it to 48 kHz.
std::vector<double> Synthesize(Channel& ch,const Parameters& p,unsigned rate,unsigned subframes) {
    unsigned count=rate/200,samples=count*subframes;
    std::array<int32_t,832> residual{};
    auto coefficients=Lpc(p.lsf,p.order),first=coefficients;
    if (p.interpolation<4) {
        std::array<int,16> interpolated{};
        for (unsigned k=0;k<p.order;++k) interpolated[k]=ch.lsf[k]+((int(p.interpolation)*(p.lsf[k]-ch.lsf[k]))>>2);
        first=Lpc(interpolated,p.order);
    }
    auto saturate=[](int64_t v) { return (int32_t)std::clamp<int64_t>(v,INT32_MIN,INT32_MAX); };
    auto pcm=[](int64_t v) { return std::clamp<int64_t>(v,-32768,32767); };
    auto multiply=[](int64_t a,int64_t b) { return (a*b)>>16; };
    uint32_t seed=p.seed;
    for (unsigned s=0;s<subframes;++s) {
        unsigned start=512+s*count;
        const auto& a=s<2?first:coefficients;
        std::array<int32_t,16> filter{};
        for (unsigned k=0;k<p.order;++k) filter[k]=(int32_t)std::lround(a[k]*4096);
        int64_t gain=(int64_t)std::llround(p.gains[s]*65536),adjustment=(ch.previousGain*65536)/gain;
        // Rescale the history when the gain changes, so LTP does not mix samples
        // captured at different gain stages.
        if (adjustment!=65536) for (unsigned i=start-16;i<start;++i)
            ch.scaled[i]=saturate(multiply(ch.scaled[i],adjustment));
        ch.previousGain=gain;
        if (p.type==2) {
            if (!s || (s==2 && p.interpolation<4)) {
                int64_t inverse=(1ll<<47)/gain;
                if (!s) inverse=multiply(inverse,(int64_t)std::lround(p.scale*16384))*4;
                for (unsigned i=start-p.pitch[s]-2;i<start;++i) {
                    int64_t value=(int64_t)std::llround(ch.pcm[i]*32768)*4096;
                    for (unsigned k=0;k<p.order;++k) value-=(int64_t)filter[k]*(int32_t)std::lround(ch.pcm[i-k-1]*32768);
                    residual[i]=saturate(multiply(inverse,pcm(Rounded(value,12))));
                }
            } else if (adjustment!=65536) {
                for (unsigned i=start-p.pitch[s]-2;i<start;++i) residual[i]=saturate(multiply(residual[i],adjustment));
            }
        }
        // Add the pseudo-random-sign excitation, then LTP (voiced) and LPC (short-term)
        // prediction, to produce the output sample.
        for (unsigned i=start;i<start+count;++i) {
            int pulse=p.pulses[i-512],offset=p.type==2?(p.offset?25:8):(p.offset?60:25);
            int64_t excitation=pulse*16384ll-Sign(pulse)*1280+offset*64;
            seed=seed*196314165u+907633515u;
            if (seed&0x80000000) excitation=-excitation;
            seed+=pulse;
            if (p.type==2) {
                int64_t prediction=2;
                for (unsigned k=0;k<5;++k) prediction+=multiply(residual[i-p.pitch[s]+2-k],(int32_t)std::lround(p.ltp[s][k]*16384));
                excitation+=2*prediction;residual[i]=saturate(excitation*2);
            }
            int64_t prediction=p.order/2;
            for (unsigned k=0;k<p.order;++k) prediction+=multiply(ch.scaled[i-k-1],filter[k]);
            ch.scaled[i]=saturate(excitation+16*prediction);
            ch.pcm[i]=pcm(Rounded(multiply(ch.scaled[i],gain>>6),8))/32768.0;
        }
    }
    std::vector<double> out(ch.pcm.begin()+512,ch.pcm.begin()+512+samples);
    std::memmove(ch.pcm.data(),ch.pcm.data()+samples,512*sizeof(double));
    std::memmove(ch.scaled.data(),ch.scaled.data()+samples,512*sizeof(int32_t));
    ch.lsf=p.lsf;ch.have=true;
    return out;
}
} // namespace

// Persistent SILK state: both channel states, stereo weights, mid/side history,
// and the resampler's allpass/FIR/delay memory (all needed to resume mid-stream).
struct OpusSilk::State {
    Channel channels[2];
    unsigned rate=0,coded=0;
    std::array<int,2> weights{};
    double mid1=0,mid2=0,side1=0;
    std::array<std::array<int32_t,6>,2> allpass{};
    std::array<std::array<int16_t,8>,2> fir{};
    std::array<std::array<int16_t,7>,2> delay{};
    unsigned inputDelay=0,ratio=0;
    double concealGain=1;
};
struct OpusSilk::Impl : State {

    // Reconfigures the decoder when the sample rate or channel count changes;
    // a rate change discards everything, a channel-count change keeps channel 0.
    void Setup(unsigned hz,unsigned count) {
        if (rate!=hz) {
            *this=Impl{};rate=hz;
            ratio=48000/hz;inputDelay=hz==8000?0:hz==12000?4:7;
        }
        if (coded!=count) {
            channels[1]=Channel{};weights={};mid1=mid2=side1=0;
            if (coded) { allpass[1]=allpass[0];fir[1]=fir[0];delay[1]=delay[0]; }
            coded=count;
        }
    }

    // Upsamples the internal 8/12/16 kHz signal to 48 kHz with the published SILK
    // resampler design (allpass cascade + 12-tap polyphase FIR), implemented here
    // generically rather than via reference routines.
    void Resample(const std::vector<double>& left,const std::vector<double>& right,std::vector<float>& out) {
#ifdef CORDINAL_CODEC_TRACE
        if (std::getenv("CORDINAL_SILK_NATIVE")) for (size_t i=0;i<left.size();++i) fprintf(stderr,"lp %.12g %.12g\n",left[i],right[i]);
#endif
        size_t start=out.size();out.resize(start+left.size()*ratio*2);
        // Published SILK filter-design coefficients, applied by this generic
        // allpass cascade and polyphase convolution. No reference routines.
        constexpr int32_t coefficients[2][3]={{1746,14986,39083},{6854,25769,55542}};
        // 16.16 phase increment per output sample; `ratio` is the integer decimation factor.
        uint32_t increment=(uint32_t)((uint64_t(rate)*2*65536+47999)/48000);
        for (unsigned ch=0;ch<2;++ch) {
            const auto& input=ch?right:left;
            std::vector<int16_t> up(input.size()*2+8);
            std::copy(fir[ch].begin(),fir[ch].end(),up.begin());
            for (size_t i=0;i<input.size();++i) {
                int16_t sample=(int16_t)std::clamp((int)std::lround(input[i]*32768),-32768,32767);
                if (inputDelay) {
                    int16_t before=delay[ch][0];
                    std::move(delay[ch].begin()+1,delay[ch].begin()+inputDelay,delay[ch].begin());
                    delay[ch][inputDelay-1]=sample;sample=before;
                }
                for (unsigned branch=0;branch<2;++branch) {
                    int64_t value=sample*1024;
                    for (unsigned section=0;section<3;++section) {
                        int32_t& state=allpass[ch][3*branch+section];
                        int64_t difference=((value-state)*coefficients[branch][section])>>16;
                        int64_t next=state+difference;
                        state=(int32_t)(value+difference);value=next;
                    }
                    up[8+i*2+branch]=(int16_t)std::clamp(Rounded(value,10),-32768,32767);
                }
            }
            uint32_t at=0;
            for (size_t i=0;i<input.size()*ratio;++i,at+=increment) {
                unsigned phase=((at&65535)*12)>>16,offset=at>>16;
                int64_t sum=0;
                for (unsigned k=0;k<8;++k) sum+=(int64_t)up[offset+k]*
                    resampler_frac_FIR_12[k<4?phase*4+k:(11-phase)*4+7-k];
                out[start+i*2+ch]=std::clamp(Rounded(sum,15),-32768,32767)*(1.0f/32768);
            }
            std::copy_n(up.end()-8,8,fir[ch].begin());
        }
    }

    bool Decode(OpusRange& r,unsigned count,unsigned hz,unsigned ms,std::vector<float>& out) {
        if (count<1 || count>2 || (hz!=8000 && hz!=12000 && hz!=16000) || (ms!=10 && ms!=20 && ms!=40 && ms!=60)) return false;
        Setup(hz,count);
        concealGain=1;
        unsigned frames=std::max(ms/20,1u),subframes=ms==10?2:4;
        // Per frame, per channel: the VAD active flag; then per channel: the LBRR
        // (low-bitrate redundancy) bitmask covering the preceding frames.
        bool active[2][3]{};unsigned lbrr[2]{};
        for (unsigned ch=0;ch<count;++ch) {
            for (unsigned f=0;f<frames;++f) active[ch][f]=r.Bit(1);
            lbrr[ch]=r.Bit(1);
        }
        if (frames>1) for (unsigned ch=0;ch<count;++ch) if (lbrr[ch])
            lbrr[ch]=r.Icdf(frames==2?Model(LBRR_flags_2_iCDF):Model(LBRR_flags_3_iCDF))+1;
        // LBRR frames carry a redundant copy of earlier frames, decoded first.
        Channel redundant[2];
        for (unsigned f=0;f<frames;++f) for (unsigned ch=0;ch<count;++ch) if (lbrr[ch]&(1u<<f)) {
            if (count==2 && !ch) { StereoWeights(r);if (!(lbrr[1]&(1u<<f))) r.Icdf(stereo_only_code_mid_iCDF); }
            bool previous=f && (lbrr[ch]&(1u<<(f-1)));
            ReadParameters(r,redundant[ch],hz,subframes,true,previous,!previous);
        }
        bool sideCoded=true;
        // Main frames: stereo weights, mid-only flag, then mid (and side) parameters.
        for (unsigned f=0;f<frames;++f) {
            auto nextWeights=count==2?StereoWeights(r):std::array<int,2>{};
            bool midOnly=count==2 && !active[1][f] && r.Icdf(stereo_only_code_mid_iCDF);
            auto midParameters=ReadParameters(r,channels[0],hz,subframes,active[0][f],f!=0,f==0);
            std::vector<double> mid=Synthesize(channels[0],midParameters,hz,subframes),side(mid.size());
            if (count==2 && !midOnly) {
                auto sideParameters=ReadParameters(r,channels[1],hz,subframes,active[1][f],f!=0 && sideCoded,f==0);
                side=Synthesize(channels[1],sideParameters,hz,subframes);
            } else if (count==2) channels[1]=Channel{};
            sideCoded=!midOnly;
            std::vector<double> left(mid.size()),right(mid.size());
            // Ramp the stereo weights over ~8 ms, then unmix mid/side to left/right
            // with the 3-tap inter-frame prediction (RFC 4.2.8).
            int ramp=8*hz/1000,step[2]{};
            for (unsigned k=0;k<2;++k) step[k]=Rounded(int64_t(nextWeights[k]-weights[k])*(65536/ramp),16);
            for (unsigned i=0;i<mid.size();++i) {
                int w0=(int)i<ramp?weights[0]+((int)i+1)*step[0]:nextWeights[0];
                int w1=(int)i<ramp?weights[1]+((int)i+1)*step[1]:nextWeights[1];
                int64_t middle=(int)std::lround(mid1*32768),sum=(int)std::lround((mid2+2*mid1+mid[i])*32768);
                int64_t prediction=(int)std::lround(side1*32768)*256ll+((sum*512*w0)>>16)+((middle*2048*w1)>>16);
                int sideSample=std::clamp(Rounded(prediction,8),-32768,32767);
                left[i]=std::clamp<int64_t>(middle+sideSample,-32768,32767)/32768.0;
                right[i]=std::clamp<int64_t>(middle-sideSample,-32768,32767)/32768.0;
                mid2=mid1;mid1=mid[i];side1=side[i];
            }
            weights=nextWeights;
#ifdef CORDINAL_CODEC_TRACE
            fprintf(stderr,"silk %u %u %u %u %08x\n",hz,count,midParameters.type,r.Tell(),r.FinalRange());
#endif
            Resample(left,right,out);
        }
        // SILK reads a fixed number of bounded symbols and permits arithmetic
        // zero extension at the end of a frame. CELT enforces its own bit budget.
        return r.Healthy();
    }

    // Packet-loss concealment: repeats the last pitch period with a decaying gain,
    // then re-runs the stereo unmix. Used when a SILK payload is empty.
    bool Conceal(unsigned count,unsigned hz,unsigned ms,std::vector<float>& out) {
        if (count<1 || count>2 || (hz!=8000 && hz!=12000 && hz!=16000) || !ms || ms>60) return false;
        Setup(hz,count);
        std::vector<double> left(ms*hz/1000),right(left.size());
        // ~0.75 decay per 20 ms, applied continuously per sample.
        const double decay=std::pow(0.75,1000.0/(hz*20));
        for (unsigned i=0;i<left.size();++i) {
            double values[2]{};
            for (unsigned ch=0;ch<count;++ch) {
                auto& state=channels[ch];
                unsigned lag=state.type==2?std::clamp(state.lag,int(hz/500),int(hz*18/1000)):hz/100;
                values[ch]=state.pcm[512-lag+i%lag]*concealGain;
            }
            double predicted=(mid2+2*mid1+values[0])*weights[0]/32768;
            left[i]=Clip((1+weights[1]/8192.0)*mid1+side1+predicted);
            right[i]=Clip((1-weights[1]/8192.0)*mid1-side1-predicted);
            mid2=mid1;mid1=values[0];side1=values[1];
            concealGain*=decay;
        }
        Resample(left,right,out);return true;
    }
};

OpusSilk::OpusSilk():impl_(std::make_unique<Impl>()) {}
OpusSilk::~OpusSilk()=default;
void OpusSilk::Reset() { *impl_=Impl{}; }
std::shared_ptr<const OpusSilk::State> OpusSilk::Save() const { return std::make_shared<State>(*impl_); }
void OpusSilk::Load(const State& state) { static_cast<State&>(*impl_)=state; }
bool OpusSilk::Decode(OpusRange& r,unsigned ch,unsigned rate,unsigned ms,std::vector<float>& out) {
    out.clear();return impl_->Decode(r,ch,rate,ms,out);
}
bool OpusSilk::Conceal(unsigned ch,unsigned rate,unsigned ms,std::vector<float>& out) {
    out.clear();return impl_->Conceal(ch,rate,ms,out);
}
} // namespace nativeaudio
