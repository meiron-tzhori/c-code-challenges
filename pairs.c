#include <stdio.h>

typedef struct _Node {
	struct _Node *next;
	int value; // simplified, instead of void *obj
} Node;

typedef struct {
	Node *head;
	Node *tail;
} List;

void print_list(List *list) {
	Node *p = list->head;
	printf("head %d tail %d list ", p->value, list->tail->value);
	while (p) {
		printf("%d ", p->value);
		p = p->next;
	}
	printf("\n");
}

void switch_pairs(List *list) {
	if (!list || !list->head || !list->tail || !list->head->next) {
		return;
	}

	Node **ppnext = &list->head; // ptr to ptr to next node
	Node *first, *second, *third;

	do {
		// before switch
		first = *ppnext;
		second = first->next;
		third = second->next;
		
		// switch pair
		*ppnext = second;
		second->next = first;
		first->next = third;
		ppnext = &first->next;
	} while (third && third->next);

	list->tail = first;
}

int main()
{
	Node n6 = {NULL, 6};
	Node n5 = {&n6, 5};
	Node n4 = {&n5, 4};
	Node n3 = {&n4, 3};
	Node n2 = {&n3, 2};
	Node n1 = {&n2, 1};
	List list = {&n1, &n6};

	print_list(&list);
	switch_pairs(&list);
	print_list(&list);

	return 0;
}

