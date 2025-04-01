#include <Arduino.h>
#include "zbitx.h"

void q_empty(struct Queue *p){
  p->head = 0;
  p->tail = 0;
  p->stall = 1;
  p->underflow = 0;
  p->overflow = 0;
}

void q_init(struct Queue *p){
  /* p->head = 0;
  p->tail = 0;
  p->stall = 1;
  p->underflow = 0;
  p->overflow = 0;*/
  p->max_q = MAX_QUEUE;
  memset(p->data, 0, p->max_q+1);
  q_empty(p);
}
#include "zbitx.h"

int q_length(struct Queue *p){
  if ( p->head >= p->tail)
    return p->head - p->tail;
  else
    return ((p->head + p->max_q) - p->tail);
}

int q_write(struct Queue *p, int32_t w){

  if (p->head + 1 == p->tail || p->tail == 0 && p->head == p->max_q-1){
    p->overflow++;
    return -1;
  }

  p->data[p->head++] = w;
  if (p->head > p->max_q){
    p->head = 0;
  }
  return 0;
}

int32_t q_read(struct Queue *p){
 int32_t data;

  if (p->tail == p->head){
    p->underflow++;
    return (int)0;
  }
    
  data = p->data[p->tail++];
  if (p->tail > p->max_q)
    p->tail = 0;

  return data;
}
