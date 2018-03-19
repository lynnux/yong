#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>

#include "gbk.h"
#include "mb.h"
#include "pinyin.h"
#include "llib.h"
#include "pyzip.h"

#define PYUNZIP_BEFORE_CMP	1

#if !PYUNZIP_BEFORE_CMP
#define LEARN_MAGIC 0x44332216
#else
#define LEARN_MAGIC 0x44332217
#endif

#define PSEARCH_PHRASE		0x00
#define PSEARCH_SENTENCE	0x01
#define PSEARCH_ALL			0x02
#define PSEARCH_PART		0x03

#define PSEARCH_BEGIN		0x01
#define PSEARCH_MID			0x02
#define PSEARCH_END			0x04
#define PSEARCH_ANY_POS		0x07

typedef struct{
	uint32_t magic;				/* �ļ�ͷ��ʶ */
	uint32_t zi_offset;			/* ���ֱ�ƫ���� */
	uint32_t zi_count;			/* �ֵ����� */
	uint32_t ci_freq;			/* ���дʶ���Ƶ�� */
	uint32_t ci_offset;			/* �ʵ�ƫ���� */
	uint32_t ci_count;			/* �ʵ����� */
	uint32_t it_offset;			/* ���ϵ�ƫ���� */
	uint32_t it_count;			/* ���ϵ����� */
	uint32_t it_phrase;			/* ���������ϵ����� */
	uint32_t raw_offset;		/* ����ƫ�� */
	uint32_t raw_size;			/* ���ݴ�С */
}LEARN_HEADER;

typedef struct{
	uint32_t code;				/* ���� */
	uint32_t cand;				/* �� */
	union{
		uint8_t lf[4];
		struct{
			uint8_t zero;		/* ����Ϊ0 */
			uint8_t nseg;		/* �����Ǽ�Ԫ�� */
			uint8_t phrase;		/* ���ϵĵ�һ�κ����һ�εĳ��ȣ�����Ϊ��λ����ռ3λ */
			uint8_t freq;		/* ���õ�Ƶ�� */
		};
	};
}LEARN_ITEM;

#ifdef TOOLS_LEARN
typedef struct{
	void *next;
	uint32_t freq;
	uint32_t part[3];
	char ci[16];
}CI_FREQ_ITEM;
#endif

typedef struct{
	char ci[12];
	uint32_t freq;
}CI_FREQ_ITEM_S;





static int ci_freq_cmp_s(const CI_FREQ_ITEM_S *v1,const CI_FREQ_ITEM_S *v2)
{
	/*int64_t ret=v1->val-v2->val;
	if(ret==0) return 0;
	else if(ret<0) return -1;
	return 1;*/
	return strcmp(v1->ci,v2->ci);
}

typedef struct{
	struct y_mb *mb;				// ��Ӧ�����
	uint32_t all_freq;				// ��Ƶ֮�ͣ�unigram����ʱ��
	uint32_t hz_freq[GB_HZ_SIZE];	// ��Ƶ��
	int ci_count;					// ��Ƶ����
	CI_FREQ_ITEM_S *ci_flat;		// ʹ����������Ĵ�Ƶ��
	LHashTable *ci_index;			// ���дʵĹ�ϣ������ֻ�ڴ������Ͽ�ʱ��
	int it_count;					// �ܵ���Ӧ������
	int it_phrase;					// ��Ԫ����������
	LEARN_ITEM *it_data;			// ����ָ��
	int raw_size;					// �ִʵ����ݴ��С
	uint8_t *raw_data;				// �ִ�����
	LEARN_ITEM key;
	void *user;
#ifdef TOOLS_LEARN
	LHashTable *ci_freq;			// ��Ƶ��
	CODE_CACHE *code_cache;
#endif
}LEARN_DATA;

static LEARN_DATA *l_predict_data;
static int l_force_mmseg;
int l_predict_simple;
int l_predict_sp;

void y_mb_learn_free(LEARN_DATA *data)
{
	if(!data) data=l_predict_data;
	if(!data) return;
#ifdef TOOLS_LEARN
	l_hash_table_free(data->ci_freq,l_free);
#endif
	l_free(data->ci_flat);
	l_hash_table_free(data->ci_index,l_free);
#ifdef TOOLS_LEARN
	code_cache_free(data->code_cache);
#endif
	free(data->it_data);
	free(data->raw_data);
	free(data);
	l_predict_data=NULL;
}

LEARN_DATA *y_mb_learn_load(struct y_mb *mb,char *in)
{
	LEARN_HEADER hdr;
	FILE *fp;
	LEARN_DATA *data;
	int ret;
	int i;
	
	fp=y_mb_open_file(in,"rb");
	if(!fp) return NULL;
	ret=fread(&hdr,1,sizeof(hdr),fp);
	if(ret!=sizeof(hdr) || hdr.magic!=LEARN_MAGIC)
	{
		fclose(fp);
		return NULL;
	}
	data=calloc(1,sizeof(LEARN_DATA));
	data->mb=mb;
	data->all_freq=hdr.ci_freq;
	
	fseek(fp,hdr.zi_offset,SEEK_SET);
	fread(data->hz_freq,sizeof(uint32_t),hdr.zi_count,fp);

	data->ci_count=hdr.ci_count;
	data->ci_flat=l_cnew0(data->ci_count,CI_FREQ_ITEM_S);
	fseek(fp,hdr.ci_offset,SEEK_SET);
	for(i=0;i<hdr.ci_count;i++)
	{
		CI_FREQ_ITEM_S *item=data->ci_flat+i;
		uint8_t len;
		fread(&len,1,1,fp);
		fread(&item->freq,4,1,fp);
		fread(&item->ci,1,len,fp);
		item->ci[len]=0;
	}

	if(hdr.it_count>0)
	{
		data->it_count=hdr.it_count;
		data->it_phrase=hdr.it_phrase;
		data->it_data=malloc(data->it_count*sizeof(LEARN_ITEM));
		if(!data->it_data)
		{
			y_mb_learn_free(data);
			return NULL;
		}
		fseek(fp,hdr.it_offset,SEEK_SET);
		fread(data->it_data,sizeof(LEARN_ITEM),data->it_count,fp);
	}
	
	if(hdr.raw_size>0)
	{
		data->raw_size=hdr.raw_size;
		data->raw_data=malloc(hdr.raw_size+256);
		if(!data->raw_data)
		{
			y_mb_learn_free(data);
			return NULL;
		}
		fseek(fp,hdr.raw_offset,SEEK_SET);
		fread(data->raw_data,1,data->raw_size,fp);
		data->key.code=data->raw_size;
	}
	
	fclose(fp);
	l_predict_data=data;
	
	if(data->ci_count<1000)
		l_force_mmseg=1;
	return data;
}

typedef struct{
	struct y_mb *mb;
	short space;
	short space2;
	py_item_t input[PY_MAX_TOKEN];
	int count;
	char *cand;
	int setence_begin;		// input�Ƿ�һ�����ӵĿ�ʼ
	int setence_end;		// input�Ƿ�һ�����ӵĽ���
	char assist_begin;		// ���׸����룬����û���õ�
	char assist_end;		// ��β������
	int mark_skip;			// ����Ƿ�������һ��ƥ���
	char last_zi[8];		// ��û�о�β�����������µ����һ����
	struct y_mb_ci *codec[1024];
}MMSEG;

#if 0
void uprintf(const char *fmt,...)
{
	va_list ap;
	char gb[256];
	char utf8[256];
	va_start(ap,fmt);
	vsprintf(gb,fmt,ap);
	va_end(ap);
	l_gb_to_utf8(gb,utf8,256);
	printf("%s",utf8);
}
#endif

static int mmseg_exist(MMSEG *mm,py_item_t *input,int count)
{
	char code[Y_MB_KEY_SIZE+1];
	struct y_mb_ci *ret;
	int len;
#ifndef TOOLS_LEARN
	const int end=(mm->assist_end && input+count==mm->input+mm->count);		
#endif

	int pos;
	pos=((input-mm->input)<<3)|count;
	ret=mm->codec[pos];
	if(ret!=(struct y_mb_ci*)-1)
		return ret?1:0;

	py_build_string(code,input,count);
	len=py_prepare_string(code,code,0);
	if(mm->space && count>1)
	{
		(void)len;
		int start=input-mm->input;
		if(start<mm->space2 && start+count>mm->space2)
		{
			// �ո�ֿ��ı��벻�ܺ�������
			mm->codec[pos]=NULL;
			return 0;
		}
	}

#ifdef TOOLS_LEARN
	ret=code_cache_test(l_predict_data->code_cache,code);
	mm->codec[pos]=ret;
	return ret?1:0;

#else
	if(l_predict_sp)
	{
		trie_tree_t *t=mm->mb->trie;
		trie_node_t *n;
		py_build_sp_string(code,input,count);
		len=py_prepare_string(code,code,0);
		n=trie_tree_get_leaf(t,code,len);
		
		// �Ҳ���ƴ��ʱ���򵥵�ѡ���Ӽ�ƴ
		if((!n || !n->data) && len==2 && code[1]=='\'')
		{
			code[1]=0;
			n=trie_tree_get_path(t,code,1);
			if(n!=NULL)
			{
				if(n->leaf)
				{
					n=trie_node_get_leaf(t,n);
				}
				else
				{
					n=trie_node_get_child(t,n);
					for(;n!=NULL;n=trie_node_get_brother(t,n))
					{
						if(n->leaf)
						{
							n=trie_node_get_leaf(t,n);
							break;
						}
					}
				}
			}					
		}
		if(!n || !n->data)
		{			
			ret=NULL;
		}
		else
		{
			ret=((struct y_mb_ci*)n->data);
			for(;ret!=NULL;ret=ret->next)
			{
				char *s;
				if(ret->del) continue;
				if(ret->len!=count*2) continue;
				s=y_mb_ci_string(ret);
				if(!(s[0]&0x80))
					continue;
				if(end && !y_mb_assist_test(mm->mb,ret,mm->assist_end,0,1))
				{
					mm->mark_skip=1;
					continue;
				}
				break;
			}
		}
	}
	else
	{
		int dlen=-1;
		if(l_predict_sp || (mm->mb->split>=2 && mm->mb->split<=4))
			dlen=count;
		ret=y_mb_code_exist(mm->mb,code,len,dlen);
		if(ret==NULL && count==1)
		{
			struct y_mb_context ctx;
			y_mb_push_context(mm->mb,&ctx);
			y_mb_set_zi(mm->mb,1);
			// FIXME: why match=0? it should be 1.
			mm->mb->ctx.result_match=0;
			if(y_mb_set(mm->mb,code,len,0)>0)
			{
				ret=mm->mb->ctx.result_first->phrase;
			}
			y_mb_pop_context(mm->mb,&ctx);
		}
		for(;ret!=NULL;ret=ret->next)
		{
			if(ret->del) continue;
			if(ret->zi && ret->len!=2)
				continue;
			if(!ret->zi && ret->ext)
				continue;
			if(ret->len!=count*2)
				continue;
			if(end && !y_mb_assist_test(mm->mb,ret,mm->assist_end,0,1))
			{
				mm->mark_skip=1;
				continue;
			}
			break;
		}
	}
	mm->codec[pos]=ret;
	return ret?1:0;
#endif
}

/*
 ��β���������
 womfuiuif
 nimfjdd
 dajwxdp
*/
static int mmseg_logcf(MMSEG *mm,int pos,int len,struct y_mb_ci **ci)
{
	char code[Y_MB_KEY_SIZE+1];
	int max=0,max2=0;
	int ext=0;
	int i;
	struct y_mb_ci /**first,*/*list,*tmp=0,*tmp2=0;
	int cpos;
	int end=((pos+len)==mm->count);
	
	cpos=(pos<<3)|len;
	list=mm->codec[cpos];
	if(!list) goto out;
	/*first=*/list=(struct y_mb_ci*)(((uintptr_t)list)&~0x03);
	
	// codeֻ�ڵ��ֵ�ʱ��ʹ��
	code[0]=0;

	if(!l_predict_data)
	{
		tmp=list;
		/*if(tmp && tmp->zi && mm->mb->simple)
		{
			// �����м���ʱ��������Ϊ�������Ƶ�Ƚϸ�
			for(list=list->next;list!=NULL;list=list->next)
			{
				if(!tmp->simp && list->simp)
				{
					tmp=list;
					break;
				}
			}
		}*/
		for(i=0,list=list->next;i<6 && list!=NULL;list=list->next)
		{
			if(l_predict_sp && list->len!=2*len)
				continue;
			if(mm->mb->split>=2 && mm->mb->split<=4 && list->len!=2*len)
				continue;
			if(end && mm->assist_end && !y_mb_assist_test(mm->mb,list,mm->assist_end,0,1))
				continue;
			if(!tmp2)
				tmp2=list;
			if(tmp->zi && mm->mb->simple && list->simp)
			{
				tmp=list;
				tmp2=tmp;
				break;
			}
		}
		goto out;
	}
	for(i=0;i<6 && list!=NULL;list=list->next)
	{
		uint32_t freq=0;

		if(list->del)
			continue;
		if(l_predict_sp && list->len!=2*len)
			continue;
		if(!list->zi && list->ext)
			continue;
		if(mm->mb->split>=2 && mm->mb->split<=4 && list->len!=2*len)
		{
			continue;
		}
		if(end && mm->assist_end && !y_mb_assist_test(mm->mb,list,mm->assist_end,0,1))
		{
			continue;
		}
		i++;
		if(len>4)
		{
		}
		else if(list->zi)
		{
			const char *s=(char*)&list->data;
			if(code[0]==0)
			{
				py_build_string(code,mm->input+pos,len);
				py_prepare_string(code,code,0);
			}
			if(y_mb_is_good_code(mm->mb,code,s))
			{			
				if(gb_is_hz((uint8_t*)s))
					freq=l_predict_data->hz_freq[GB_HZ_OFFSET(s)];
				if(pos==0 && mm->setence_begin)
					freq=freq&1023;
				else if(end && mm->setence_end)
					freq=(freq>>21)&1023;
				else
					freq=(freq>>10)&2047;
			}
		}
		else
		{
			const char *s=y_mb_ci_string(list);
			freq=0;
			if(len<=4)
			{
				CI_FREQ_ITEM_S *found;
				found=bsearch(s,l_predict_data->ci_flat,
						l_predict_data->ci_count,
						sizeof(CI_FREQ_ITEM_S),
						(LCmpFunc)ci_freq_cmp_s);
				if(found)
				{
					freq=found->freq;
					if(pos==0 && mm->setence_begin)
						freq=freq&1023;
					else if(end && mm->setence_end)
						freq=(freq>>21)&1023;
					else
						freq=(freq>>10)&2047;
				}
			}
		}

		/* û�и�����ʱ����Ƶ���е�ʱ��˳�� */
		if(mm->assist_end && end)
		{
			if(!tmp)
			{
				max=freq;
				ext=list->ext;
				tmp=list;
			}
			else if(!tmp2)
			{
				max2=freq;
				tmp2=list;
			}
		}
		else
		{
			if(!tmp || freq>max || (ext && !list->ext))
			{
				max=freq;
				ext=list->ext;
				tmp=list;
			}
		}
	}
	// ������β����ʱ�������õڶ���ƥ��Ĵ�
out:
	if(mm->assist_end && end && tmp2 && len>=1)
	{
		const char *s=y_mb_ci_string(tmp);
		// ֻ������ѡ��tmp��ĩ����ͬ�Ż��ɵڶ���ѡ
		//printf("%s %s\n",s,mm->last_zi);
		int last_len=(int)strlen(mm->last_zi);
		int cmp_len=(tmp->len>=last_len)?last_len:tmp->len;
		if(!strcmp(s+tmp->len-cmp_len,mm->last_zi+last_len-cmp_len))
		{
			tmp=tmp2;
			max=max2;
		}
	}
	
	//printf("%s %d\n",y_mb_ci_string(tmp),max);
	if(ci) *ci=tmp;
	if(l_predict_data)
		max-=l_predict_data->all_freq;
	return max;
}

static int ci_freq_get(const char *s)
{
	CI_FREQ_ITEM_S *item;
	if(!l_predict_data) return 0;
	item=bsearch(s,l_predict_data->ci_flat,
			l_predict_data->ci_count,
			sizeof(CI_FREQ_ITEM_S),(LCmpFunc)ci_freq_cmp_s);
	if(!item) return 0;
	return item->freq;
}

#if !PYUNZIP_BEFORE_CMP
static int predict_compar_phrase(const LEARN_ITEM *p1, const LEARN_ITEM *p2)
{
	char *s1,*s2;
	s1=(char*)l_predict_data->raw_data+p1->code;
	s2=(char*)l_predict_data->raw_data+p2->code;
	return strcmp(s1,s2);
}

static int predict_compar_part(const LEARN_ITEM *p1, const LEARN_ITEM *p2)
{
	char *s1,*s2;
	int n;
	s1=(char*)l_predict_data->raw_data+p1->code;
	s2=(char*)l_predict_data->raw_data+p2->code;
	if(!p1->cand)
	{
		n=strlen(s1);
	}
	else
	{
		n=strlen(s2);
	}
	return strncmp(s1,s2,n);
}
#else
static int predict_compar_phrase(const LEARN_ITEM *p1, const LEARN_ITEM *p2)
{
	char temp[256];
	char *s1,*s2;
	if(!p1->cand)
	{
		s1=(char*)l_predict_data->raw_data+p1->code;
		cp_unzip((char*)l_predict_data->raw_data+p2->code,temp);
		s2=temp;
	}
	else
	{
		s2=(char*)l_predict_data->raw_data+p2->code;
		cp_unzip((char*)l_predict_data->raw_data+p1->code,temp);
		s1=temp;
	}
	return strcmp(s1,s2);
}

static int predict_compar_part(const LEARN_ITEM *p1, const LEARN_ITEM *p2)
{
	char temp[256];
	char *s1,*s2;
	int n;
	if(!p1->cand)
	{
		s1=(char*)l_predict_data->raw_data+p1->code;
		cp_unzip((char*)l_predict_data->raw_data+p2->code,temp);
		s2=temp;
		n=strlen(s1);
	}
	else
	{
		s2=(char*)l_predict_data->raw_data+p2->code;
		cp_unzip((char*)l_predict_data->raw_data+p1->code,temp);
		s1=temp;
		n=strlen(s2);
	}
	return strncmp(s1,s2,n);
}
#endif

static int predict_compar_part_cand(const LEARN_ITEM *p1, const LEARN_ITEM *p2)
{
	char *s1,*s2;
	int n;
	s1=(char*)l_predict_data->raw_data+p1->cand;
	s2=(char*)l_predict_data->raw_data+p2->cand;
	if(!p1->cand)
	{
		n=strlen(s1);
	}
	else
	{
		n=strlen(s2);
	}
	return strncmp(s1,s2,n);
}

static int predict_test_assist(MMSEG *mm,LEARN_ITEM *item,int end)
{
	char temp[MAX_CAND_LEN+1];
	int len;
	
	if(!mm || !mm->assist_end || !end)
		return 1;
	len=cz_unzip((const char*)(l_predict_data->raw_data+item->cand),temp,sizeof(temp));
	if(len<2) return 1;
	return y_mb_assist_test_hz(mm->mb,temp+len-2,mm->assist_end);
}

static LEARN_ITEM *predict_search(LEARN_DATA *data,MMSEG *mm,const char *code,int flag,int *count,int end)
{
	LEARN_ITEM *key=&data->key;
	LEARN_ITEM *item=NULL,*pnext=NULL;
	char *temp;
	if(!data)
		return NULL;
	temp=(char*)data->raw_data+key->code;
	key->cand=0;

#if !PYUNZIP_BEFORE_CMP
	cp_zip(code,temp);
#else
	if(temp!=code)
		strcpy(temp,code);
#endif

	{
		int i,num;
		i=l_bsearch_left(key,data->it_data,data->it_count,sizeof(LEARN_ITEM),
				(LCmpFunc)(flag==PSEARCH_PHRASE?predict_compar_phrase:predict_compar_part));
		for(num=0;i<data->it_count;i++)
		{
			LEARN_ITEM *p;
			p=data->it_data+i;
			if((flag==PSEARCH_PHRASE || flag==PSEARCH_SENTENCE) && predict_compar_phrase(p,key)!=0)
			{
				break;
			}
			else if(predict_compar_part(p,key)!=0)
				break;
			if(p->lf[1]<1 && flag==PSEARCH_PHRASE)
				continue;
			if(mm->space2 && (p->lf[2]>>3) && (p->lf[2]>>3)>mm->space2)
				continue;
			if(predict_test_assist(mm,p,end)==1)
			{
				num++;
				if(!item)
					item=data->it_data+i;
				else if(num==2 && mm!=NULL)
					pnext=data->it_data+i;
			}
		}
		if(count) *count=num;
	}
	if(mm && flag==PSEARCH_PHRASE && end && item && mm->assist_end)
	{
		mm->mark_skip=1;
		return pnext;
	}

	return item;
}

static int predict_copy(LEARN_DATA *data,char *dst,LEARN_ITEM *src,int count)
{
	const char *part=(const char*)data->raw_data+src->cand;
	return cz_unzip(part,dst,count);
}

#if 1
// ����ֲ����Ž�
static uint32_t unigram_split[64][64];
static struct y_mb_ci *unigram_codec[1024];

// ����ִ�Ƶ
static int unigram_logcf(MMSEG *mm,int pos,int len,struct y_mb_ci **ci)
{
	int ret;
	if(len>7 || !mmseg_exist(mm,mm->input+pos,len))
	{
		return 0;
	}
	else
	{
		ret=mmseg_logcf(mm,pos,len,ci);
	}
	if(l_predict_data)
	{
		return -ret;
	}
	else
	{
		// TODO: û�����Ͽ��ʱ����ô��
	}
	return ret;
}

// ά�ر��㷨
static uint32_t unigram_best(MMSEG *mm,int b,int l)
{
	uint32_t res;
	int i;
	struct y_mb_ci *c;
	res=unigram_split[b][l];
	if(res!=0)
		return res;
	res=unigram_logcf(mm,b,l,&c);
	if(res>0 && c)
	{
		res=res<<6|l;
		unigram_codec[b<<3|l]=c;
		goto out;
		//if(l==1)
		//	goto out;
		//printf("R %d %d %d %d\n",b,l,res&0x3f,res>>6);
	}
	if(l==1)
	{
		//printf("unigram best can't found %d %d\n",b,l);
		return 0;
	}

	for(i=1;i<l;i++)
	{
		uint32_t res1,res2,temp;
		res1=unigram_best(mm,b,i);
		if(res1==0) return 0;
		res2=unigram_best(mm,b+i,l-i);
		if(res2==0) return 0;
		temp=(res1>>6)+(res2>>6);
		if(res==0 || temp<res>>6)
		{
			res=i|(temp<<6);
		}
	}
out:
	//printf("%d %d %d %d\n",b,l,res&0x3f,res>>6);
	unigram_split[b][l]=res;
	return res;
}

// ����һ���з�����
static int unigram_build(int b,int l,uint8_t *out,int p)
{
	uint32_t res;
	int len;
	res=unigram_split[b][l];
	assert(res>0);
	len=(int)(res&0x3f);
	assert(len>0 && len<=l);
	if(len==l)
	{
		out[p++]=(int)l;
	}
	else
	{
		p=unigram_build(b,len,out,p);
		p=unigram_build(b+len,l-len,out,p);
	}
	return p;
}

// �������
static void unigram_output(MMSEG *mm,uint8_t *seq,int len,char *out)
{
	int i;
	int pos;
	//for(i=0;i<len;i++)printf("%d\n",seq[i]);
	for(i=0,pos=0;i<len;)
	{
		struct y_mb_ci *c;
#if 1
		int j;
		for(j=4;j>=2;j--)
		{
			int tlen=0,k;
			LEARN_ITEM *item,*key;
			char *predict,*temp;
			if(i+j>len)
			{
				continue;
			}
			key=&l_predict_data->key;
			temp=(char*)l_predict_data->raw_data+key->code;			
			for(k=0;k<j;k++) tlen+=seq[i+k];			
			py_build_string(temp,mm->input+pos,tlen);
			py_prepare_string(temp,temp,0);
			item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,pos+tlen==mm->count);
			if(!item)
			{
				continue;
			}
			predict=mm->cand+strlen(mm->cand);
			predict_copy(l_predict_data,predict,item,-1);
			if((item->lf[2]&0x07)>0)
			{
				if((item->lf[2]&0x07)<seq[i+j-1])
				{
					int delta=seq[i+j-1]-(item->lf[2]&0x07);
					seq[i+j-1]-=delta;
					seq[i+j-2]+=delta;
				}
				else if((item->lf[2]&0x07)==seq[i+j-1]+1 && seq[i+j-2]>1)
				{
					// ֻ�����Ĭ�Ϸָ��һ���֣�����һ���ָ���ֹһ���ֵ����
					seq[i+j-2]--;
					seq[i+j-1]++;
				}
			}
			if(i+j<len)
			{
				int tlen2=0;
				LEARN_ITEM *item2;
				for(k=0;k<j;k++) tlen2+=seq[i+1+k];
				py_build_string(temp,mm->input+pos+seq[i],tlen2);
				py_prepare_string(temp,temp,0);
				item2=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,i+j+1==len);
				// �����������ȼ������غ����ݲ�һ���������ڵ�ǰλ�ý��в���
				if(item2!=NULL && item2->lf[3]>item->lf[3])
				{
					predict_copy(l_predict_data,temp,item,-1);
					if(memcmp(predict+2*seq[i],temp,(tlen-seq[i])*2))
					{
						predict[0]=0;
						break;
					}
				}
			}
			
			i+=j;
			pos+=tlen;
			
			// ѭ����������
			while(i<len)		// û�е���β
			{
				// ����k��ʾjԪ��ĺ󼸸���
				for(k=j-1;k>=1;k--)
				{
					// base��k���ʵı���������ext��ʾ�����õ��Ķ�����������
					int base,t,ext=1;
					for(t=0,base=0;t<k;t++) base+=seq[i-k+t];
					// prefix��k���ʵ��ı�
					char *prefix=predict+strlen(predict)-2*base;
					py_build_string(temp,mm->input+pos-base,base+seq[i]);
					py_prepare_string(temp,temp,0);
					item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,pos+seq[i]==mm->count);
					if(!item && i+1<len)
					{
						py_build_string(temp,mm->input+pos-base,base+seq[i]+seq[i+1]);
						py_prepare_string(temp,temp,0);
						item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,pos+seq[i]+seq[i+1]==mm->count);
						if(item) ext++;
					}
					if(!item) continue;
					predict_copy(l_predict_data,temp,item,-1);
					if(memcmp(prefix,temp,2*base))
					{
						continue;
					}
					strcpy(mm->cand+strlen(mm->cand),temp+2*base);
					pos+=seq[i];
					if(ext>1) pos+=seq[i+1];
					i+=ext;
					j=k+ext;
					break;
				}
				if(k==0) break;
			}
			
			goto next;
		}
#endif
		c=unigram_codec[pos<<3|seq[i]];
		//fprintf(stderr,"%d %d %d %p %d\n",i,pos,seq[i],c,pos<<3|seq[i]);
		strcat(out,y_mb_ci_string(c));
		pos+=seq[i];
		i++;
next:;
	}
	//printf("%s\n",out);
}

static int unigram(MMSEG *mm)
{
	uint8_t seq[64];
	int len;
	
	//clock_t start=clock();
	
	memset(mm->codec,-1,sizeof(mm->codec));
	memset(unigram_split,0,sizeof(unigram_split));
	mm->mark_skip=0;
	
	if(0==unigram_best(mm,0,mm->count))
	{
		//printf("find best fail\n");
		return -1;
	}
	len=unigram_build(0,mm->count,seq,0);
	
#if 1
	int i,pos;
	for(i=0,pos=0;i<len-1;i++)
	{
		struct y_mb_ci *c;
		int freq1,freq2;
		if(seq[i]!=1)
		{
			pos+=seq[i];
			continue;
		}
		if(seq[i+1]!=1)
		{
			pos+=seq[i];
			continue;
		}
		c=mm->codec[pos<<3|2];
		if(c==(void*)-1)
		{
			mmseg_exist(mm,mm->input+pos,2);
			c=mm->codec[pos<<3|2];
		}
		if(c==NULL)
		{
			pos+=seq[i];
			continue;
		}
		freq1=(unigram_best(mm,pos,1)>>6)+(unigram_best(mm,pos+1,1)>>6);
		freq2=unigram_best(mm,pos,2)>>6;
		//printf("%d %d\n",freq1,freq2);
		
		if(i<len-2 && seq[i+2]==1)
		{
			if(freq2-freq2/10>freq1)
			{
				pos+=seq[i];
				continue;
			}
		}
		else if(freq2-freq2/25>freq1)
		{
			pos+=seq[i];
			continue;
		}
		//fprintf(stdout,"%d %d %x %s\n",i,pos,(pos<<3|2),y_mb_ci_string(c));
		seq[i]=2;
		memmove(seq+i+1,seq+i+2,len-i-2);
		len--;
		pos+=seq[i];
	}
#endif
	unigram_output(mm,seq,len,mm->cand);
	
	//printf("%.3f\n",(clock()-start)*1.0/CLOCKS_PER_SEC);

	return 0;
}

#endif

static int mmseg_split(MMSEG *mm)
{
	int mmcount;					/* ���п��ܵ���Ԫ������� */
	uint8_t mmlen[80];				/* һ����Ԫ�����ϵĳ��� */
	uint8_t mmword[80];			/* ��Ԫ���һԪ�ĳ��� */
	uint8_t mmword2[80];			/* ��Ԫ��ǰ��Ԫ�ĳ��� */
	uint8_t mmnword[80];			/* ��ѡ����ж���Ԫ */
	int mmlogcf[80];			/* ��Ӧ����ʹʵĴ�Ƶ */
	int mmlogcf2[80];			/* ǰ��Ԫ�Ĵ�Ƶ */
	uint16_t mmavglen[80];			/* ��Ԫ���ƽ������ */
	float mmvarlen[80];			/* ��Ԫ��ĳ��ȱ仯 */
	uint8_t mmlong[80];			/* ��ѡ��Ԫ�������һ�� */
	struct y_mb_ci *mmfirst[80];	/* ��Ӧ��Ƶ����ѡ�� */
	int longest;					/* ��ǰ��ĺ�ѡ��Ԫ�� */
	int i,k,l;
	int result=0;

	mm->mark_skip=0;
    memset(mm->codec,-1,sizeof(mm->codec));

    for(i=0;i<mm->count;)
    {
        /* �õ���iλ�ÿ�ʼ����Ԫ�飬�����ͳ����Ϣ */
		int w[3];
		
		longest=0;
		mmcount=0;
		/* w[0]������ǰ�棬����ʹ�õ�һ�������΢�е����ȶ� */
		for(w[0]=7;w[0]>=1;w[0]--){
		if(i+w[0] > mm->count) continue;
		if(!mmseg_exist(mm,mm->input+i,w[0])) continue;
		for(w[1]=7;w[1]>=0;w[1]--){
		if(i+w[0]+w[1] > mm->count) continue;
		if(w[1] && !mmseg_exist(mm,mm->input+i+w[0],w[1]))
		{
			/* ��ʹ���ֲ����ڣ���Ȼ������ѯ�������
			 * ���������뷨��˵�����ֱ�Ȼ���ڣ�����һ���жϷ����������뷨���ٶ�����
			if(w[1]==1)
			{
				return -1;
			}
			*/
			continue;
		}
		for(w[2]=7;w[2]>=0;w[2]--){
		if(i+w[0]+w[1]+w[2]>mm->count) continue;
		if(!w[1] && w[2]) continue;
		if(w[2] && !mmseg_exist(mm,mm->input+i+w[0]+w[1],w[2]))
		{
			/*
			if(w[2]==1)
			{
				return -2;
			}
			*/
			continue;
		}
		{
			float sumsq=0;
			uint16_t avglen;
			
			mmword[mmcount]=w[0];
			mmword2[mmcount]=w[0]+w[1];
			mmlen[mmcount]=0;
			mmnword[mmcount]=0;
			mmavglen[mmcount]=0;
			mmlogcf[mmcount]=0;
			mmlogcf2[mmcount]=0;
			mmfirst[mmcount]=0;
			
			for(k=0;k<3;k++)
				mmlen[mmcount]+=w[k];
			if(mmlen[mmcount]<longest)
				continue;
			if(mmlen[mmcount]>longest)
			{
				mmlen[0]=mmlen[mmcount];
				mmcount=0;
				mmword[mmcount]=w[0];
				mmword2[mmcount]=w[0]+w[1];
				mmnword[mmcount]=0;
				mmavglen[mmcount]=0;
				mmlogcf[mmcount]=0;
			}

			for(k=0;k<3;k++)
			{
				if(w[k]>0)
				{
					struct y_mb_ci *ci;
					int pos=i;
					int logcf;
					if(k>=1) pos+=w[0];
					if(k>=2) pos+=w[1];
					logcf=mmseg_logcf(mm,pos,w[k],&ci);			
					mmlogcf[mmcount]+=logcf;
					if(k<2) mmlogcf2[mmcount]+=logcf;
					if(k==0)
					{
						//assert(ci!=NULL);
						mmfirst[mmcount]=ci;
					}
					mmnword[mmcount]++;
				}
			}
			mmavglen[mmcount]=avglen=(uint16_t)(mmlen[mmcount]*100/mmnword[mmcount]);
			for(k=0;k<mmnword[mmcount];k++)
				sumsq+=(float) (w[k] - avglen/100.0) * (w[k] - avglen/100.0);
			mmvarlen[mmcount]=sumsq;
			if(mmlen[mmcount]>longest)
				longest=mmlen[mmcount];
			
			/* ���������ֻʣ��һ���ʣ�ֱ������ͺã�ʹ�����Ͽⷴ������ */	
			if(!w[1])
			{
				struct y_mb_ci *c=mmfirst[mmcount];
				y_mb_ci_string2(c,mm->cand+strlen(mm->cand));
				return result;
			}
			mmcount++;
			//printf("%d %d: %d %d %d %d %.2f\n",i,mmcount,w[0],w[1],w[2],mmlogcf[mmcount-1],mmavglen[mmcount-1]/100.0);
			
			/* ��������ѭ������Ϊ��ѭ��������Ĵ��ˣ����̵�û������ */
			break;
		}}}}
        if(longest==0)
        {
            /* һ����û�ҵ���һ�����Ϸ������봮 */
            return result;
        }
        /* �õ�����󳤶ȵĵ��б� */
        for(l=k=0;k<mmcount;k++)
        {
            if(mmlen[k]==longest)
            {
                mmlong[l]=k;
                l++;
            }
		}
		if(l>0 && l_predict_data && l_predict_data->it_count>0)
        {
			LEARN_ITEM *key=&l_predict_data->key;
			LEARN_ITEM *item,*prev=NULL;
			int pos,which,full=1;
			int prev_count=0;
			char *temp=(char*)l_predict_data->raw_data+key->code;
			for(k=0;k<l;k++)
			{
				int count=mmlen[mmlong[k]];
				if(count==prev_count)
				{
					/* �п��ܳ��ּ��������ͬһ��������������������²��ظ����� */
					continue;
				}
				prev_count=count;
				py_build_string(temp,mm->input+i,count);
				py_prepare_string(temp,temp,0);
				item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,i+count==mm->count);
				if(!item) continue;
				if(!prev || item->lf[3]>prev->lf[3])
				{
					pos=0;
					prev=item;
					which=mmlong[k];
				}
			}
			if(!prev && longest>=5 && i+longest<mm->count)
			{
				/* ����򵥴���ĳЩ��Ԫ�飬�����ǲ�����Ƕ�׵����� */
				int count=longest+1;
				py_build_string(temp,mm->input+i,count);
				py_prepare_string(temp,temp,0);
				item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,i+count==mm->count);
				if(item && item->lf[1]==3)
				{
					char *predict=mm->cand+strlen(mm->cand);
					predict_copy(l_predict_data,predict,item,-1);
					i+=longest+1;
					continue;
				}
			}
			if(!prev && longest>=4)
			{
				full=0;
				if(longest==4)
				{
					/* �г��ô�2+2�����ٳ��Բ���������ƥ�� */
					for(k=0;k<l;k++)
					{
						if(mmword[k]==2 && mmword2[k]==4)
							goto skip_2_2;
					}
				}
				for(k=0;k<l;k++)
				{
					int count=mmnword[mmlong[k]];
					if(count!=3) continue;
					py_build_string(temp,mm->input+i,mmword2[mmlong[k]]);
					py_prepare_string(temp,temp,0);
					item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,0);
					if(item && (!prev || item->lf[3]>prev->lf[3]))
					{
						pos=0;
						prev=item;
						which=mmlong[k];
					}
					py_build_string(temp,mm->input+i+mmword[mmlong[k]],mmlen[mmlong[k]]-mmword[mmlong[k]]);
					py_prepare_string(temp,temp,0);
					item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,i+mmlen[mmlong[k]]==mm->count);
					if(item && (!prev || item->lf[3]>prev->lf[3]))
					{
						pos=1;
						prev=item;
						which=mmlong[k];
					}
				}
				skip_2_2:;
			}
			if(prev) /* �����ҵ������п����Ե������� */
			{
				/* last: ���һ���ʵ���ʼλ�� base: ���һ���ʵĳ��� */
				int last,base;
				char *prefix,*predict;
				
				if(pos==0)
				{
					predict=mm->cand+strlen(mm->cand);
					predict_copy(l_predict_data,predict,prev,-1);
					
					if(mmlen[which]==mmword2[which] || !full)
					{
						last=i+mmword[which];
						base=mmword2[which]-mmword[which];
					}
					else
					{
						last=i+mmword2[which];
						base=mmlen[which]-mmword2[which];
					}
					if(full)
						i+=mmlen[which];
					else
						i+=mmword2[which];
				}
				else
				{
					/* 23λ�������ϣ�����ֻӦ����ѡ���һ��������34Ҳ�����ϵĻ���������޷�����Ƶ����Ϣ�� */
					y_mb_ci_string2(mmfirst[which],mm->cand+strlen(mm->cand));
					i+=mmword[which];
					continue;
				}
				//printf("find in dict %s\n",predict);
				
				/* �ݹ�����Ƿ��к͵�ǰ������ƥ��Ķ�Ԫ�� */
				prefix=predict+strlen(predict)-2*base;
				while(prefix>predict && mm->count>last+base)
				{
					//printf("prefix %s\n",prefix);	
					k=MIN(7,mm->count-last-base);
					for(;k>0;k--)
					{
						py_build_string(temp,mm->input+last,base+k);
						py_prepare_string(temp,temp,0);
						//printf("%d %s\n",k,temp);
						item=predict_search(l_predict_data,mm,temp,PSEARCH_PHRASE,0,last+base+k==mm->count);
						if(!item) continue;
						predict_copy(l_predict_data,temp,item,-1);
						if(memcmp(prefix,temp,2*base))
						{
							continue;
						}
						predict=temp;
						break;
					}
					if(k==0) break;
					//printf("predict %s\n",predict);
					strcpy(mm->cand+strlen(mm->cand),predict+2*base);
					last+=base;
					base=k;
					i+=k;
					//prefix=predict+strlen(predict)-2*base;
					prefix=mm->cand+strlen(mm->cand)-2*base;
				}
				continue;
			}
		}

        if(l==1)
        {
            /* ����Ӧ�����ƥ��ԭ�� */
            int mmword_len=mmword[mmlong[0]];
            y_mb_ci_string2(mmfirst[mmlong[0]],mm->cand+strlen(mm->cand));
			i+=mmword_len;
			//printf("find in long %d %s\n",mmword_len,y_mb_ci_string(mmfirst[mmlong[0]]));
        }
        else
        {
            /* Ӧ��ƽ���ʳ�ԭ�� */
            float lavg=0;
            int n=0;
            uint8_t largeavg[100];
            for(k=0;k<l;k++)
            {
                if(mmavglen[mmlong[k]]>lavg)
                    lavg=mmavglen[mmlong[k]];
            }
			for(k=0;k<l;k++)
			{
				if(mmavglen[mmlong[k]]==lavg)
				{
					largeavg[n]=mmlong[k];
					n++;
				}
			}
            if(n==1)
            {
				int mmword_len=mmword[largeavg[0]];
				y_mb_ci_string2(mmfirst[largeavg[0]],mm->cand+strlen(mm->cand));
				i+=mmword_len;
				//printf("find in avg %d %.2f %s\n",mmword_len,lavg,y_mb_ci_string(mmfirst[largeavg[0]]));
            }
            else
            {
                /* Ӧ�ôʳ��Ľ�С������ԭ�� */
                float svar=100;
                uint8_t smallvar[100];
                int o;
                for(k=0;k<n;k++)
                {
                    if(mmvarlen[largeavg[k]]<svar)
                        svar=mmvarlen[largeavg[k]];
                }
                for(o=k=0;k<n;k++)
                {
					//��С������ԭ�򾭳��õ��������������Ľ����������������ʱ����
                    //if(mmvarlen[largeavg[k]]==svar)
                    {
                        smallvar[o]=largeavg[k];
                        o++;
                    }
                }
                if(o==1)
                {
					int mmword_len=mmword[smallvar[0]];
					y_mb_ci_string2(mmfirst[smallvar[0]],mm->cand+strlen(mm->cand));
					i+=mmword_len;
					//printf("find in var %d %s\n",mmword_len,y_mb_ci_string(mmfirst[smallvar[0]]));
                }
                else
                {
                    /* Ӧ�������ִ�Ƶlogcf�ĺ�ԭ�� */
                    int llog=-1000000;
                    uint8_t largelog=0;
                    char *out=mm->cand+strlen(mm->cand);
                    int mmword_len;
                    
                    for(k=0;k<o;k++)
                    {
                        if(mmlogcf[smallvar[k]]>llog)
                            llog=mmlogcf[smallvar[k]];
                    }
					for(k=0;k<o;k++)
					{
						if(mmlogcf[smallvar[k]]==llog)
						{
							largelog=smallvar[k];
							break;
						}
					}
					/*if(mmword2[largelog]==4 && mmword[largelog]==2)
					{
						if(!mmseg_fix(mm,i,4,mmlogcf2[largelog],out))
						{
							i+=4;
							continue;
						}
					}*/
					mmword_len=mmword[largelog];
					y_mb_ci_string2(mmfirst[largelog],out);
					i+=mmword_len;
					//printf("find in freq %d %d %s\n",mmword_len,llog,y_mb_ci_string(mmfirst[largelog]));
                }
            }
        }
    }
    return result;
}

const char *y_mb_predict_nth(const char *s,int n)
{
	int i;
	for(i=0;i<n;i++)
	{
		int len=strlen(s);
		assert(len>0);
		s+=len+1;
	}
	return s;
}

static int get_space_pos(MMSEG *mm,const char *s)
{
	int pos=0;
	const char *p;
	p=strchr(s,' ');
	if(p)
	{
		struct y_mb *mb=mm->mb;
		int i,split;
		pos=(int)(p-s);
		for(i=split=0;i<pos;i++)
			if(s[i]==mb->split) split++;
		pos-=split;
	}
	return pos;
}

static inline int zrm_csh_mohu(int in,int out)
{
	if(in=='c' && out=='i')
		return 1;
	if(in=='s' && out=='u')
		return 1;
	if(in=='z' && out=='v')
		return 1;
	return 0;
}

struct _p_item{struct y_mb_ci *c;int f;int m;};
static int _p_item_cmpar(struct _p_item *it1,struct _p_item *it2)
{int m=it2->m-it1->m;if(m) return m;return it2->f-it1->f;}
static int predict_quanpin_simple(struct y_mb *mb,py_item_t *item,int count,char *out,int *out_len)
{
	int ret,len;
	char temp[128];
	trie_iter_t iter;
	trie_tree_t *trie;
	trie_node_t *n;
	int depth;
	char *c;
	LArray *array;

	if(!(trie=mb->trie) || count<2)
		return 0;
	depth=py_build_sp_string(temp,item,count);
	if(!(c=strchr(temp,'\'')) || !c[1])
		return 0;
	
	array=l_array_new(26,sizeof(struct _p_item));
retry:
	n=trie_iter_path_first(&iter,trie,NULL,64);
	while(n!=NULL)
	{
		int cur=iter.depth;
		if(cur<depth)
		{
			//printf("%d %c\n",cur,temp[cur]);
			int c=temp[cur];
			if(n->self!=c && c!='\'' && ((cur&0x01)!=0 || !zrm_csh_mohu(c,n->self)))
			{
				trie_iter_path_skip(&iter);
				n=trie_iter_path_next(&iter);
				continue;
			}
		}
		if(cur>=depth-1 && n->leaf)
		{
			struct y_mb_ci *ci=trie_node_get_leaf(trie,n)->data;
			for(;ci!=NULL;ci=ci->next)
			{
				struct _p_item item;
				if(ci->del || ci->zi || ci->len!=cur+1)
					continue;
				c=y_mb_ci_string(ci);
				item.c=ci;
				item.f=(l_predict_data && ci->len<15)?ci_freq_get(c):0;
				item.m=(ci->len==depth);
				l_array_insert_sorted(array,&item,(LCmpFunc)_p_item_cmpar);
				if(!l_predict_data && array->len>25)
					array->len=25;
				if(l_predict_data && array->len>10)
					array->len=10;
			}
		}
		n=trie_iter_path_next(&iter);
	}
	if(array->len==0)
	{
		// FIXME: ���ﻹ��ֻ�ܽ�����ֵ�����
		c=strpbrk(temp,"uvi");
		if(c!=NULL && c[1]=='\'')
		{
			memmove(c+4,c+2,strlen(c+2)+1);
			if(c[0]=='u') c[0]='s';
			else if(c[0]=='v') c[0]='z';
			else c[0]='c';
			c[1]='\'';
			c[2]='h';
			c[3]='\'';
			goto retry;
		}
	}
	for(ret=len=0;ret<array->len;ret++)
	{
		struct _p_item *it=l_array_nth(array,ret);
		struct y_mb_ci *ci=it->c;
		if(len+ci->len+1+1>MAX_CAND_LEN)
			break;
		c=y_mb_ci_string(ci);
		y_mb_ci_string2(ci,out+len);
		len+=ci->len+1;
	}
	out[len]=0;
	l_array_free(array,NULL);
	if(out_len)
		*out_len=len+1;
	return ret;
}

static int y_mb_find_setence(MMSEG *mm,const char *code)
{
	typedef struct{
		LEARN_ITEM *p;
		int len;
	}RES_ITEM;
	#define MATCH_COUNT		16

	RES_ITEM lst[MATCH_COUNT];
	int lcnt=0;
	
	LEARN_ITEM *item;
	int i,count,len,tmp;
	int first_match=-1;
	int assist_end;
	int cp_len;
	char temp[128];
	
	if(!l_predict_data)
		return 0;

	assist_end=mm->assist_end;
	mm->assist_end=0;

#if !PYUNZIP_BEFORE_CMP
	cp_zip(code,temp);
	cp_len=strlen(temp);
#else
	cp_len=strlen(code);
#endif
	
	item=predict_search(l_predict_data,mm,code,PSEARCH_ALL,&count,1);
	if(item!=NULL) for(i=0;i<count && lcnt<MATCH_COUNT;i++,item++)
	{
		int res_len=-1;
#if !PYUNZIP_BEFORE_CMP
		char *item_code=(char*)l_predict_data->raw_data+item->code;
		if(item_code[cp_len]!=0)
		{
			res_len=mm->count;
		}
#else
		cp_unzip((char*)l_predict_data->raw_data+item->code,temp);
		if(temp[cp_len]!=0)
		{
			res_len=mm->count;
		}
#endif
		if(assist_end)
		{
			int templen;
			templen=predict_copy(l_predict_data,temp,item,res_len);
			if(y_mb_assist_test_hz(mm->mb,temp+templen-2,assist_end)==0)
				continue;
		}
		lst[lcnt++]=(RES_ITEM){item,res_len};
		if(first_match==-1)
			first_match=i;
	}
	
	mm->assist_end=assist_end;
	
	for(i=0,tmp=0,len=0;i<lcnt;i++)
	{
		int templen,j;
		if(mm->assist_end && i==0 && first_match==0)
			continue;
		if(mm->count<5 && lst[i].len>=0)
			continue;
		for(j=0;j<i;j++)
		{
			if(0==predict_compar_part_cand(lst[i].p,lst[j].p))
			{
				break;
			}
		}
		if(i!=0 && j<i)
			continue;
		templen=predict_copy(l_predict_data,temp,lst[i].p,lst[i].len);
		if(tmp+templen+1>MAX_CAND_LEN)
			break;
		strcpy(mm->cand+tmp,temp);
		tmp+=templen+1;
		len++;
	}
	if(!mm->assist_end && mm->count>=5) for(i=0;i<lcnt;i++)
	{
		int templen;
		if(lst[i].len<=0) continue;
		templen=predict_copy(l_predict_data,temp,lst[i].p,-1);
		if(templen==2*lst[i].len) continue;
		if(tmp+templen+1>MAX_CAND_LEN)
			break;
		strcpy(mm->cand+tmp,temp);
		tmp+=templen+1;
		len++;
	}

	if(len && (!mm->assist_end || first_match>=0))
		return len;
	mm->cand[0]=0;
	return 0;
}

int y_mb_predict_by_learn(struct y_mb *mb,char *s,int caret,char *out,int size,int begin)
{
	MMSEG mm;
	int len;
	int tmp;
	char temp[256];
	
	char simple_data[256];
	int simple_count=0;
	int simple_size;

	mm.mb=mb;
	mm.setence_begin=begin;
	mm.setence_end=(s[caret]==0);
	mm.assist_begin=0;
	mm.assist_end=0;
	tmp=s[caret];s[caret]=0;

	if(l_predict_sp)
	{
		py_prepare_string(temp,s,0);
		if(l_predict_simple)
		{
			mm.count=py_parse_sp_simple(s,mm.input);
			if(mm.count>1)
				simple_count=predict_quanpin_simple(mb,mm.input,mm.count,simple_data,&simple_size);
			else
				simple_count=y_mb_predict_simple(mb,temp,simple_data,&simple_size,l_predict_data?ci_freq_get:0);
			if(simple_count>0)
			{
				memcpy(out,simple_data,simple_size);
				s[caret]=tmp;
				return simple_count;
			}
		}
		len=py_conv_from_sp(s,temp,sizeof(temp),'\'');
		if(tmp==0)
		{
			int assist=s[caret-1];
			if(!islower(assist) && !(assist==';' && py_sp_has_semi()))
			{
				mm.assist_end=s[caret-1];
			}
			else if(len>=3 && temp[len-1]=='\'')
			{
				mm.assist_end=temp[len-2];
				temp[len-2]=0;
			}
		}
		mm.space=get_space_pos(&mm,temp);
		mm.count=py_parse_string(temp,mm.input,-1);
		if(mm.count<=0)
			return 0;
		mm.count=py_remove_split(mm.input,mm.count);
		mm.space2=py_get_space_pos(mm.input,mm.count,mm.space);
		if(mm.assist_end)
		{
			char last[16];
			py_build_string(last,mm.input+mm.count-1,1);
			if(!y_mb_check_assist(mb,last,strlen(last),mm.assist_end,1))
			{
				mm.assist_end=0;
				len=py_conv_from_sp(s,temp,sizeof(temp),'\'');
				mm.space=get_space_pos(&mm,temp);
				mm.count=py_parse_string(temp,mm.input,-1);
				mm.count=py_remove_split(mm.input,mm.count);
				mm.space2=py_get_space_pos(mm.input,mm.count,mm.space);
			}
		}
	}
	else
	{
		mm.space=get_space_pos(&mm,s);
		mm.count=py_parse_string(s,mm.input,-1);
		mm.count=py_remove_split(mm.input,mm.count);
		mm.space2=py_get_space_pos(mm.input,mm.count,mm.space);
	}
	s[caret]=tmp;

	if(mm.count<=1)
	{
		return 0;
	}
	
	if(mm.space>0)
	{
		assert(mm.space2);
	}
	
	py_build_string(temp,mm.input,mm.count);
	py_prepare_string(temp,temp,0);

	if(!l_predict_sp && l_predict_simple)
	{
		if(mb->trie)
			simple_count=predict_quanpin_simple(mb,mm.input,mm.count,simple_data,&simple_size);
		else
			simple_count=y_mb_predict_simple(mb,temp,simple_data,&simple_size,l_predict_data?ci_freq_get:0);
		if(simple_count>0)
		{
			memcpy(out,simple_data,simple_size);
			return simple_count;
		}
	}

	mm.cand=out;
	mm.cand[0]=0;

	tmp=mb->match;
	mb->match=1;

	/* ��˫ƴ������ĩβ��������ĸ������£�ǿ����Ϊ�Ǿ�β������ */
	if((mb->split==2 || l_predict_sp ) && mm.count>2 && !mm.assist_end && s[caret]=='\0')
	{
		char last[16];
		if(l_predict_sp)
		{
			py_build_sp_string(last,mm.input+mm.count-1,1);
			if(last[1]=='\'')
				last[1]=0;
		}
		else
		{
			py_build_string(last,mm.input+mm.count-1,1);
		}
		py_prepare_string(last,last,0);
		if(strlen(last)==1)
		{
			int assist=last[0];
			py_build_string(last,mm.input+mm.count-2,1);
			if(y_mb_check_assist(mb,last,strlen(last),assist,1))
			{
				mm.assist_end=assist;
				mm.count--;
				temp[strlen(temp)-1]=0;
			}
		}
	}

	len=y_mb_find_setence(&mm,temp);
	if(len>0) return len;

	if(mm.assist_end)
	{
		int temp=mm.assist_end;
		int count;
		mm.assist_end=0;
		mm.last_zi[0]=0;
		if(l_predict_data!=NULL && !l_force_mmseg)
			count=unigram(&mm);
		else
			count=mmseg_split(&mm);
		len=strlen(mm.cand);
		if(len>=4)
		{
			mm.assist_end=temp;
			strcpy(mm.last_zi,mm.cand+len-4);
			mm.cand[0]=0;
			if(l_predict_data!=NULL && !l_force_mmseg)
				mm.count=unigram(&mm);
			else
				mm.count=mmseg_split(&mm);
		}
		else
		{
			mm.count=count;
		}
	}
	else
	{
		mm.last_zi[0]=0;
		if(l_predict_data!=NULL && !l_force_mmseg)
		{
			mm.count=unigram(&mm);
		}
		else
		{
			mm.count=mmseg_split(&mm);
		}
	}

	len=strlen(mm.cand);
	mb->match=tmp;
	
	if(gb_strlen((uint8_t*)mm.cand)==1)
	{
		out[0]=0;
		len=0;
	}
	if(mm.count>0)
		return mm.count;
	
	return len>0?1:0;
}


